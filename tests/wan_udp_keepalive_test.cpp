// Own loopback UDP sink observes the production timer/writer. Malformed input
// is injected at the registration boundary; no firmware peer or radio session.
// Reference: published FlexLib 4.3.16 Radio.UdpRegistrationLoop and its
// _udpPacketPipeline Flex-OUI preamble qualification; ClientHandleHelper.X8.
#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/RadioConnection.h"
#include "TestSettingsProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMetaObject>
#include <QNetworkDatagram>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <cstdio>
#include <vector>

namespace AetherSDR {
class WanUdpKeepaliveTestAccess {
public:
    static QTimer& timer(PanadapterStream& stream) { return *stream.m_wanRegisterTimer; }
    static bool confirmed(const PanadapterStream& stream) { return stream.m_wanRegistered; }
    static void feed(PanadapterStream& stream, const QByteArray& packet,
                     const QHostAddress& sender, quint16 port)
    {
        stream.confirmWanRegistration(packet, sender, port);
    }
    static void timeout(PanadapterStream& stream)
    {
        // Exercise the connected callback even if a timeout was delivered late.
        QMetaObject::invokeMethod(stream.m_wanRegisterTimer, "timeout", Qt::DirectConnection);
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
void pump(int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
}
std::vector<QByteArray> drain(QUdpSocket& sink)
{
    std::vector<QByteArray> packets;
    while (sink.hasPendingDatagrams()) {
        packets.push_back(sink.receiveDatagram().data());
    }
    return packets;
}
QByteArray meterPacket()
{
    QByteArray packet(32, '\0');
    auto* raw = reinterpret_cast<uchar*>(packet.data());
    // ExtDataWithStream, class, integer + fractional timestamps, 8 words.
    qToBigEndian<quint32>(0x38D00008u, raw);
    qToBigEndian<quint32>(0x70000001u, raw + 4);
    qToBigEndian<quint32>(0x001C2Du, raw + 8);
    qToBigEndian<quint32>(0x534C8002u, raw + 12);
    qToBigEndian<quint16>(1, raw + 28);
    return packet;
}
bool allEqual(const std::vector<QByteArray>& packets, const QByteArray& expected)
{
    for (const QByteArray& packet : packets) {
        if (packet != expected) {
            return false;
        }
    }
    return true;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("wan-udp-keepalive"));
    if (!profile.isValid()) {
        return 77;
    }
    QCoreApplication app(argc, argv);
    QUdpSocket sink;
    QUdpSocket replacement;
    if (!sink.bind(QHostAddress::LocalHost, 0)
        || !replacement.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind own loopback UDP sinks\n");
        return 77;
    }
    PanadapterStream stream;
    stream.init();
    stream.startWanUdpRegister(0xAu);
    check(!WanUdpKeepaliveTestAccess::timer(stream).isActive(), "cold registration cannot start a timer");
    if (!stream.startWan(QHostAddress::LocalHost, sink.localPort())) {
        std::fprintf(stderr, "SKIP: cannot bind production VITA socket\n");
        return 77;
    }
    stream.startWanUdpRegister(0);
    pump(100);
    check(drain(sink).empty(), "wait for a nonzero client handle");
    const qint64 bytesBefore = stream.totalTxBytes();
    const QByteArray expected("client udp_register handle=0x0000000A");
    stream.startWanUdpRegister(0xAu);
    pump(10);
    std::vector<QByteArray> packets = drain(sink);
    check(packets.size() == 1 && allEqual(packets, expected), "immediate registration uses eight uppercase hex digits");
    check(WanUdpKeepaliveTestAccess::timer(stream).interval() == 50, "fast timer interval is 50 ms");
    pump(225);
    packets = drain(sink);
    check(packets.size() >= 3 && packets.size() <= 6 && allEqual(packets, expected), "real fast timer repeatedly sends udp_register");
    check(stream.totalTxBytes() - bytesBefore == qint64((packets.size() + 1) * expected.size()), "successful registration writes count bytes exactly");

    const QByteArray valid = meterPacket();
    const auto feed = [&](const QByteArray& packet, const QHostAddress& sender, quint16 port) {
        WanUdpKeepaliveTestAccess::feed(stream, packet, sender, port);
        check(!WanUdpKeepaliveTestAccess::confirmed(stream)
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 50,
              "malformed or unrelated datagram leaves registration fast");
    };
    feed(QByteArray(1, '\0'), QHostAddress::LocalHost, sink.localPort());
    feed(QByteArray(28, '\0'), QHostAddress::LocalHost, sink.localPort());
    feed(valid.left(16), QHostAddress::LocalHost, sink.localPort());
    QByteArray wrongOui = valid;
    wrongOui[10] = 0;
    feed(wrongOui, QHostAddress::LocalHost, sink.localPort());
    QByteArray noClass = valid;
    noClass[0] = char(0x30);
    feed(noClass, QHostAddress::LocalHost, sink.localPort());
    QByteArray wrongType = valid;
    wrongType[0] = char(0x28);
    feed(wrongType, QHostAddress::LocalHost, sink.localPort());
    QByteArray missingTimestamp = valid.left(16);
    missingTimestamp[3] = 4;
    feed(missingTimestamp, QHostAddress::LocalHost, sink.localPort());
    QByteArray missingTrailer = valid.left(28);
    missingTrailer[0] = char(0x3C);
    missingTrailer[3] = 7;
    feed(missingTrailer, QHostAddress::LocalHost, sink.localPort());
    feed(valid, QHostAddress(QStringLiteral("127.0.0.2")), sink.localPort());
    feed(valid, QHostAddress::LocalHost, replacement.localPort());
    pump(110);
    packets = drain(sink);
    check(packets.size() >= 1 && allEqual(packets, expected), "fast production sends continue after refused datagrams");

    // Also exercise the socket-to-qualification wiring with first datagrams.
    sink.writeDatagram(QByteArray(1, '\0'), QHostAddress::LocalHost, stream.localPort());
    QByteArray unrelated = valid;
    unrelated[15] = char(0x06);  // unhandled Flex class; routing has no effect
    replacement.writeDatagram(unrelated, QHostAddress::LocalHost, stream.localPort());
    pump(110);
    check(!WanUdpKeepaliveTestAccess::confirmed(stream)
          && WanUdpKeepaliveTestAccess::timer(stream).interval() == 50,
          "actual malformed and foreign first datagrams cannot confirm WAN registration");
    packets = drain(sink);
    check(!packets.empty() && allEqual(packets, expected), "real receive path keeps rapid registration alive");

    // Positive receive travels through QUdpSocket::readyRead and onDatagramReady.
    sink.writeDatagram(valid, QHostAddress::LocalHost, stream.localPort());
    pump(25);
    check(WanUdpKeepaliveTestAccess::confirmed(stream)
          && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
          "valid Flex VITA from the session endpoint selects five-second cadence");
    check(stream.categoryStats(PanadapterStream::CatMeter).packets == 1,
          "qualification preserves production meter routing");
    drain(sink);
    pump(300);
    check(drain(sink).empty(), "confirmation stops fast sends");
    // Observe two actual five-second timeouts; continued receive must not reset them.
    QElapsedTimer steadyClock;
    steadyClock.start();
    qint64 firstAt = -1;
    qint64 secondAt = -1;
    int steadyCount = 0;
    while (steadyClock.elapsed() < 10500 && steadyCount < 2) {
        pump(10);
        packets = drain(sink);
        for (const QByteArray& packet : packets) {
            check(packet == expected, "steady production payload stays udp_register");
            if (++steadyCount == 1) {
                firstAt = steadyClock.elapsed();
                sink.writeDatagram(valid, QHostAddress::LocalHost, stream.localPort());
            } else {
                secondAt = steadyClock.elapsed();
            }
        }
    }
    check(steadyCount == 2 && firstAt >= 4300 && firstAt <= 5500
          && secondAt - firstAt >= 4800 && secondAt - firstAt <= 5500,
          "actual steady keepalives recur every five seconds without receive postponement");

    // Qualification follows the preamble, independently of stream dispatch:
    // both data-with-stream types, optional timestamps and trailers are legal.
    std::vector<QByteArray> preambles;
    QByteArray typeOne = valid;
    typeOne[0] = char(0x18);
    preambles.push_back(typeOne);
    QByteArray noTimestamps = valid.left(16);
    noTimestamps[1] = 0;
    noTimestamps[3] = 4;
    preambles.push_back(noTimestamps);
    QByteArray trailer = valid + QByteArray(4, '\0');
    trailer[0] = char(0x3C);
    trailer[3] = 9;
    preambles.push_back(trailer);
    for (const QByteArray& preamble : preambles) {
        stream.startWanUdpRegister(0xAu);
        WanUdpKeepaliveTestAccess::feed(stream, preamble, QHostAddress::LocalHost, sink.localPort());
        check(WanUdpKeepaliveTestAccess::confirmed(stream)
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "valid alternative Flex preamble qualifies before stream dispatch");
    }
    pump(10);
    drain(sink);

    stream.startWanUdpRegister(0xABCDEF01u);
    pump(120);
    packets = drain(sink);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0xABCDEF01"))
          && !WanUdpKeepaliveTestAccess::confirmed(stream), "handle replacement restarts fast cadence without old payloads");
    stream.stop();
    drain(sink);
    const qint64 stoppedBytes = stream.totalTxBytes();
    WanUdpKeepaliveTestAccess::timeout(stream);
    stream.startWanUdpRegister(0xAu);
    pump(120);
    check(drain(sink).empty() && stream.totalTxBytes() == stoppedBytes
          && !WanUdpKeepaliveTestAccess::timer(stream).isActive(), "stop fences late timeouts and late handle delivery");

    check(stream.startWan(QHostAddress::LocalHost, replacement.localPort()), "reconnect binds the replacement destination");
    pump(100);
    check(drain(replacement).empty(), "reconnect cannot reuse the previous handle");
    stream.startWanUdpRegister(0xBu);
    pump(120);
    packets = drain(replacement);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0x0000000B"))
          && drain(sink).empty(), "reconnect sends only new handle to new destination");
    WanUdpKeepaliveTestAccess::feed(stream, valid, QHostAddress::LocalHost, sink.localPort());
    check(!WanUdpKeepaliveTestAccess::confirmed(stream), "old endpoint cannot confirm the new session");

    // Replacing a running WAN destination clears the handle before any send.
    check(stream.startWan(QHostAddress::LocalHost, sink.localPort()), "active WAN destination can be replaced");
    pump(110);
    check(drain(sink).empty() && drain(replacement).empty(), "destination replacement retires old timer and handle");
    stream.startWanUdpRegister(0xCu);
    pump(110);
    packets = drain(sink);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0x0000000C")),
          "replacement destination uses only the replacement handle");

    // Transition back to LAN preserves its priming and has no WAN timer.
    RadioConnection conn;
    check(stream.start(&conn), "WAN to LAN binds normally");
    WanUdpKeepaliveTestAccess::timeout(stream);
    check(!WanUdpKeepaliveTestAccess::timer(stream).isActive()
          && drain(replacement).empty(), "LAN start retires WAN registration");
    check(stream.rebindToEphemeralPort(&conn), "LAN collision rebind remains usable");
    check(stream.startWan(QHostAddress::LocalHost, sink.localPort()), "restart WAN before collision rebind");
    stream.startWanUdpRegister(0xDu);
    pump(10);
    drain(sink);
    check(stream.rebindToEphemeralPort(&conn), "WAN to LAN rebind remains usable");
    WanUdpKeepaliveTestAccess::timeout(stream);
    pump(110);
    check(!WanUdpKeepaliveTestAccess::timer(stream).isActive() && drain(sink).empty(),
          "collision rebind retires WAN timer and destination");
    stream.stop();
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
