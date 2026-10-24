// Socket-free: inject the terminal LAN write, then drive the production keepalive
// timer and session lifecycle. No firmware peer or radio. The destination port is
// the radio's measured LAN VITA source port (FLEX-8600 fw 4.3.16).
#include "core/backends/flex/PanadapterStream.h"
#include "TestSettingsProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
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
        stream.m_lanWriteForTest = std::move(write);
        stream.m_wanBindForTest = [] { return true; };
        stream.m_wanWriteForTest = [](const QByteArray& payload, const QHostAddress&, quint16) {
            return qint64(payload.size());
        };
    }
    // The LAN bind path needs a RadioConnection and a real socket; arm the
    // keepalive with the state start() leaves behind instead.
    static void armLan(PanadapterStream& stream, const QHostAddress& radio)
    {
        stream.m_isWanMode = false;
        stream.m_radioAddress = radio;
        stream.startLanKeepalive();
    }
    static QTimer& timer(PanadapterStream& stream) { return *stream.m_lanKeepaliveTimer; }
    static void markReceived(PanadapterStream& stream) { stream.m_hasReceivedPacket = true; }
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
    std::vector<Datagram> recorded;
    PanadapterStream stream;
    LanUdpKeepaliveTestAccess::inject(stream,
        [&recorded](const QByteArray& payload, const QHostAddress& destination, quint16 port) -> qint64 {
            recorded.push_back({payload, destination, port});
            return payload.size();
        });
    stream.init();

    LanUdpKeepaliveTestAccess::armLan(stream, QHostAddress());
    check(recorded.empty() && !LanUdpKeepaliveTestAccess::timer(stream).isActive(),
          "no radio address: no keepalive and no timer");

    for (const QHostAddress& radio : {QHostAddress(QStringLiteral("192.168.50.254")),
                                      QHostAddress(QStringLiteral("fd58:eee:46e8::10"))}) {
        recorded.clear();
        const qint64 bytesBefore = stream.totalTxBytes();
        LanUdpKeepaliveTestAccess::armLan(stream, radio);
        check(recorded.size() == 1 && recorded[0].payload == ping
              && recorded[0].destination == radio && recorded[0].port == 4993,
              "start sends one 1-byte keepalive to the radio's VITA source port 4993");
        check(LanUdpKeepaliveTestAccess::timer(stream).isActive()
              && LanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "keepalive timer runs every 5 s");

        LanUdpKeepaliveTestAccess::markReceived(stream);
        LanUdpKeepaliveTestAccess::timeout(stream);
        check(recorded.size() == 2 && recorded[1].payload == ping && recorded[1].port == 4993,
              "keepalive continues after the first VITA packet arrives");
        check(LanUdpKeepaliveTestAccess::timer(stream).isActive(),
              "first packet does not stop the keepalive timer");
        check(stream.totalTxBytes() - bytesBefore == 2, "keepalive bytes are counted");

        stream.stop();
        check(!LanUdpKeepaliveTestAccess::timer(stream).isActive(), "stop() stops the keepalive");
        LanUdpKeepaliveTestAccess::timeout(stream);
        check(recorded.size() == 2, "a late timeout after stop() sends nothing");
    }

    recorded.clear();
    LanUdpKeepaliveTestAccess::armLan(stream, QHostAddress(QStringLiteral("192.168.50.254")));
    check(stream.startWan(QHostAddress::LocalHost, 4993), "injected WAN bind succeeds");
    check(!LanUdpKeepaliveTestAccess::timer(stream).isActive(), "switching to WAN stops the LAN keepalive");
    recorded.clear();
    LanUdpKeepaliveTestAccess::timeout(stream);
    check(recorded.empty(), "WAN mode never sends the LAN keepalive");
    stream.stop();

    std::printf("%s lan_udp_keepalive_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
