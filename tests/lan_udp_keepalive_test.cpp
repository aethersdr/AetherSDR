// Socket-free: inject the terminal LAN bind and write, then drive the production
// start(), rebindToEphemeralPort(), receive path and keepalive timer. No firmware
// peer or radio. 4993 is the radio's measured LAN VITA source port (fw 4.3.16).
#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/RadioConnection.h"
#include "TestSettingsProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QNetworkDatagram>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace AetherSDR {
class LanUdpKeepaliveTestAccess {
public:
    static void inject(PanadapterStream& stream,
                       std::function<qint64(const QByteArray&, const QHostAddress&, quint16)> write)
    {
        stream.m_lanBindForTest = [] { return true; };
        stream.m_lanWriteForTest = std::move(write);
        stream.m_wanBindForTest = [] { return true; };
        stream.m_wanWriteForTest = [](const QByteArray& payload, const QHostAddress&, quint16) {
            return qint64(payload.size());
        };
    }
    static void setRadioAddress(RadioConnection& conn, const QHostAddress& radio)
    {
        conn.m_radioAddr = radio;
    }
    static bool rebind(PanadapterStream& stream, RadioConnection* conn)
    {
        return stream.rebindToEphemeralPort(conn);
    }
    static void receive(PanadapterStream& stream, const QByteArray& payload,
                        const QHostAddress& from, quint16 port)
    {
        QNetworkDatagram dg(payload);
        dg.setSender(from, port);
        stream.handleReceivedDatagram(dg);
    }
    static QTimer& timer(PanadapterStream& stream) { return *stream.m_lanKeepaliveTimer; }
    static void timeout(PanadapterStream& stream)
    {
        QMetaObject::invokeMethod(stream.m_lanKeepaliveTimer, "timeout", Qt::DirectConnection);
    }
};
}
using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}
struct Datagram {
    QByteArray payload;
    QHostAddress destination;
    quint16 port;
};
// Keepalives only; the 4992 registration prime shares the injected write.
std::vector<Datagram> keepalives(const std::vector<Datagram>& recorded)
{
    std::vector<Datagram> out;
    for (const Datagram& d : recorded) {
        if (d.port != 4992)
            out.push_back(d);
    }
    return out;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("lan-udp-keepalive"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    const QByteArray ping(1, '\x00');
    const QByteArray vita(PanadapterStream::VITA49_HEADER_BYTES, '\x00');
    std::vector<Datagram> recorded;
    qint64 writeResult = 0;  // 0 = echo the payload size
    PanadapterStream stream;
    LanUdpKeepaliveTestAccess::inject(stream,
        [&](const QByteArray& payload, const QHostAddress& destination, quint16 port) -> qint64 {
            recorded.push_back({payload, destination, port});
            return writeResult ? writeResult : payload.size();
        });
    stream.init();
    RadioConnection conn;

    check(stream.start(&conn) && keepalives(recorded).empty()
              && !LanUdpKeepaliveTestAccess::timer(stream).isActive(),
          "start() without a radio address: no keepalive and no timer");
    stream.stop();

    for (const QHostAddress& radio : {QHostAddress(QStringLiteral("192.168.50.254")),
                                      QHostAddress(QStringLiteral("fd58:eee:46e8::10"))}) {
        LanUdpKeepaliveTestAccess::setRadioAddress(conn, radio);
        recorded.clear();
        const qint64 bytesBefore = stream.totalTxBytes();
        check(stream.start(&conn), "start() binds through the injected LAN seam");
        auto sent = keepalives(recorded);
        check(sent.size() == 1 && sent[0].payload == ping
              && sent[0].destination == radio && sent[0].port == 4993,
              "start() sends one 1-byte keepalive to the radio's VITA source port 4993");
        check(LanUdpKeepaliveTestAccess::timer(stream).isActive()
              && LanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "start() runs the keepalive timer every 5 s");

        LanUdpKeepaliveTestAccess::receive(stream, vita, radio, 4993);
        check(stream.hasReceivedPackets(), "the first VITA packet is recorded");
        LanUdpKeepaliveTestAccess::timeout(stream);
        sent = keepalives(recorded);
        check(LanUdpKeepaliveTestAccess::timer(stream).isActive()
              && sent.size() == 2 && sent[1].port == 4993,
              "keepalive continues after the first VITA packet arrives");
        check(stream.totalTxBytes() - bytesBefore == qint64(recorded.size()),
              "keepalive and prime bytes are counted");

        recorded.clear();
        check(LanUdpKeepaliveTestAccess::rebind(stream, &conn),
              "rebindToEphemeralPort() binds through the injected LAN seam");
        sent = keepalives(recorded);
        check(sent.size() == 1 && sent[0].port == 4993 && sent[0].destination == radio
              && LanUdpKeepaliveTestAccess::timer(stream).isActive(),
              "rebindToEphemeralPort() re-arms the keepalive at 4993");

        recorded.clear();
        writeResult = -1;
        const qint64 bytesBeforeFail = stream.totalTxBytes();
        LanUdpKeepaliveTestAccess::timeout(stream);
        LanUdpKeepaliveTestAccess::timeout(stream);
        writeResult = 0;
        check(stream.totalTxBytes() == bytesBeforeFail
              && LanUdpKeepaliveTestAccess::timer(stream).isActive(),
              "a failed send counts no bytes and keeps the timer running");
        LanUdpKeepaliveTestAccess::timeout(stream);
        check(stream.totalTxBytes() - bytesBeforeFail == 1, "the next successful send counts again");

        recorded.clear();
        stream.stop();
        check(!LanUdpKeepaliveTestAccess::timer(stream).isActive(), "stop() stops the keepalive");
        LanUdpKeepaliveTestAccess::timeout(stream);
        check(recorded.empty(), "a late timeout after stop() sends nothing");
    }

    check(stream.start(&conn), "start() before switching to WAN");
    check(stream.startWan(QHostAddress::LocalHost, 4993), "injected WAN bind succeeds");
    check(!LanUdpKeepaliveTestAccess::timer(stream).isActive(), "switching to WAN stops the LAN keepalive");
    recorded.clear();
    LanUdpKeepaliveTestAccess::timeout(stream);
    check(recorded.empty(), "WAN mode never sends the LAN keepalive");
    stream.stop();

    std::printf("%s lan_udp_keepalive_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
