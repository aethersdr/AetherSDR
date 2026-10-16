// Opt-in check of production UDP wiring, separate from default socket-free
// cadence/qualification/lifecycle coverage. No firmware peer or radio session.
#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/RadioConnection.h"
#include "TestSettingsProfile.h"

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

namespace AetherSDR {
class WanUdpKeepaliveSocketTestAccess {
public:
    static QTimer& timer(PanadapterStream& stream) { return *stream.m_wanRegisterTimer; }
    static bool confirmed(const PanadapterStream& stream) { return stream.m_wanRegistered; }
    static void timeout(PanadapterStream& stream)
    {
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
int drain(QUdpSocket& sink, const QByteArray& expected)
{
    int count = 0;
    while (sink.hasPendingDatagrams()) {
        check(sink.receiveDatagram().data() == expected, "native UDP payload is padded udp_register");
        ++count;
    }
    return count;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("wan-udp-keepalive-socket"));
    if (!profile.isValid()) {
        return 77;
    }
    QCoreApplication app(argc, argv);
    QUdpSocket outgoing;
    QUdpSocket incoming;
    if (!outgoing.bind(QHostAddress::LocalHost, 0) || !incoming.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind own loopback sinks\n");
        return 77;
    }
    PanadapterStream stream;
    stream.init();
    if (!stream.startWan(QHostAddress::LocalHost, outgoing.localPort())) {
        std::fprintf(stderr, "SKIP: cannot bind production UDP socket\n");
        return 77;
    }
    const QByteArray expected("client udp_register handle=0x0000000A");
    stream.startWanUdpRegister(0xAu);
    pump(20);
    check(drain(outgoing, expected) >= 1, "production writer reaches the configured sink");

    QByteArray packet(32, '\0');
    auto* raw = reinterpret_cast<uchar*>(packet.data());
    qToBigEndian<quint32>(0x38D00008u, raw);
    qToBigEndian<quint32>(0x70000001u, raw + 4);
    qToBigEndian<quint32>(0x001C2Du, raw + 8);
    qToBigEndian<quint32>(0x534C8002u, raw + 12);
    qToBigEndian<quint16>(1, raw + 28);
    check(incoming.writeDatagram(packet, QHostAddress::LocalHost, stream.localPort()) == packet.size(),
          "own incoming socket delivers a Flex VITA fixture");
    pump(100);
    check(WanUdpKeepaliveSocketTestAccess::confirmed(stream)
          && WanUdpKeepaliveSocketTestAccess::timer(stream).interval() == 5000,
          "readyRead qualifies VITA from a different source endpoint");
    check(stream.categoryStats(PanadapterStream::CatMeter).packets == 1,
          "native receive wiring preserves meter dispatch");
    drain(outgoing, expected);
    pump(120);
    check(drain(outgoing, expected) == 0, "native confirmation ends rapid sends");

    RadioConnection conn; // cold, never dials
    check(stream.start(&conn), "WAN to LAN binds normally");
    WanUdpKeepaliveSocketTestAccess::timeout(stream);
    pump(120);
    check(!WanUdpKeepaliveSocketTestAccess::timer(stream).isActive()
          && drain(outgoing, expected) == 0, "LAN start retires WAN registration");
    check(stream.rebindToEphemeralPort(&conn), "LAN collision rebind remains usable");
    stream.stop();
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
