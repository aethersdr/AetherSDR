// Socket-free: inject terminal bind/write operations, then drive the production
// timer, encoder, qualifier and session lifecycle. No firmware peer or radio.
// Reference: published FlexLib 4.3.16 Radio.UdpRegistrationLoop and its
// _udpPacketPipeline Flex-OUI preamble qualification; ClientHandleHelper.X8.
#include "core/backends/flex/PanadapterStream.h"
#include "TestSettingsProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <functional>
#include <vector>
#include <utility>

namespace AetherSDR {
class WanUdpKeepaliveTestAccess {
public:
    static void inject(PanadapterStream& stream, std::function<bool()> bind,
                       std::function<qint64(const QByteArray&, const QHostAddress&, quint16)> write)
    {
        stream.m_wanBindForTest = std::move(bind);
        stream.m_wanWriteForTest = std::move(write);
    }
    static bool nativeSocketUnopened(const PanadapterStream& stream)
    {
        return stream.m_socket && stream.m_socket->socketDescriptor() == -1;
    }
    static QTimer& timer(PanadapterStream& stream) { return *stream.m_wanRegisterTimer; }
    static bool confirmed(const PanadapterStream& stream) { return stream.m_wanRegistered; }
    static void feed(PanadapterStream& stream, const QByteArray& packet)
    {
        stream.confirmWanRegistration(packet);
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
struct Datagram {
    QByteArray payload;
    QHostAddress destination;
    quint16 port;
};
std::vector<QByteArray> drain(std::vector<Datagram>& recorded, quint16 port)
{
    std::vector<QByteArray> packets;
    for (auto it = recorded.begin(); it != recorded.end();) {
        if (it->port == port) {
            check(it->destination == QHostAddress(QHostAddress::LocalHost), "writer uses configured address");
            packets.push_back(it->payload);
            it = recorded.erase(it);
        } else {
            ++it;
        }
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
        std::fprintf(stderr, "FAIL: cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    constexpr quint16 kSinkPort = 4993;
    constexpr quint16 kReplacementPort = 4994;
    std::vector<Datagram> recorded;
    bool bindSucceeds = true;
    bool writeSucceeds = true;
    PanadapterStream stream;
    WanUdpKeepaliveTestAccess::inject(stream, [&bindSucceeds] { return bindSucceeds; },
        [&recorded, &writeSucceeds](const QByteArray& payload, const QHostAddress& destination, quint16 port) -> qint64 {
            if (!writeSucceeds) {
                return -1;
            }
            recorded.push_back({payload, destination, port});
            return payload.size();
        });
    stream.init();
    stream.startWanUdpRegister(0xAu);
    check(!WanUdpKeepaliveTestAccess::timer(stream).isActive(), "cold registration cannot start a timer");
    if (!stream.startWan(QHostAddress::LocalHost, kSinkPort)) {
        std::fprintf(stderr, "FAIL: injected bind should succeed\n");
        return 1;
    }
    // The UDP registration encoder already follows ClientHandleHelper.X8.
    // Keep its full uint32 contract aligned with the NetCW production captures.
    struct HandleCase { quint32 value; const char* payload; };
    const HandleCase handles[] = {
        {0x0000000Au, "client udp_register handle=0x0000000A"},
        {0x0012ABCDu, "client udp_register handle=0x0012ABCD"},
        {0x1234ABCDu, "client udp_register handle=0x1234ABCD"},
        {0x80000001u, "client udp_register handle=0x80000001"},
        {0xFFFFFFFFu, "client udp_register handle=0xFFFFFFFF"},
    };
    for (const HandleCase& handle : handles) {
        stream.startWanUdpRegister(handle.value);
        const std::vector<QByteArray> captured = drain(recorded, kSinkPort);
        check(captured.size() == 1 && allEqual(captured, QByteArray(handle.payload)),
              "registration preserves the full uint32 handle in exact X8 command framing");
    }
    stream.startWanUdpRegister(0);
    pump(100);
    check(drain(recorded, kSinkPort).empty(), "wait for a nonzero client handle");
    const qint64 bytesBefore = stream.totalTxBytes();
    const QByteArray expected("client udp_register handle=0x0000000A");
    stream.startWanUdpRegister(0xAu);
    std::vector<QByteArray> packets = drain(recorded, kSinkPort);
    check(packets.size() == 1 && allEqual(packets, expected), "immediate registration uses eight uppercase hex digits");
    check(WanUdpKeepaliveTestAccess::timer(stream).interval() == 50
          && WanUdpKeepaliveTestAccess::timer(stream).timerType() == Qt::PreciseTimer,
          "fast timer uses precise 50 ms retries");
    pump(225);
    packets = drain(recorded, kSinkPort);
    check(!packets.empty() && allEqual(packets, expected), "production fast timer repeatedly sends udp_register");
    check(stream.totalTxBytes() - bytesBefore == qint64((packets.size() + 1) * expected.size()), "successful registration writes count bytes exactly");

    const QByteArray valid = meterPacket();
    const auto feed = [&](const QByteArray& packet) {
        WanUdpKeepaliveTestAccess::feed(stream, packet);
        check(!WanUdpKeepaliveTestAccess::confirmed(stream)
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 50,
              "malformed or non-Flex datagram leaves registration fast");
    };
    feed(QByteArray(1, '\0'));
    feed(QByteArray(28, '\0'));
    feed(valid.left(16));
    QByteArray wrongOui = valid;
    wrongOui[10] = 0;
    feed(wrongOui);
    QByteArray noClass = valid;
    noClass[0] = char(0x30);
    feed(noClass);
    QByteArray wrongType = valid;
    wrongType[0] = char(0x28);
    feed(wrongType);
    QByteArray missingTimestamp = valid.left(16);
    missingTimestamp[3] = 4;
    feed(missingTimestamp);
    pump(110);
    packets = drain(recorded, kSinkPort);
    check(packets.size() >= 1 && allEqual(packets, expected), "fast production sends continue after refused datagrams");

    WanUdpKeepaliveTestAccess::feed(stream, valid);
    pump(25);
    check(WanUdpKeepaliveTestAccess::confirmed(stream)
          && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000
          && WanUdpKeepaliveTestAccess::timer(stream).timerType() == Qt::CoarseTimer,
          "valid Flex VITA selects five-second cadence");
    drain(recorded, kSinkPort);
    pump(300);
    check(drain(recorded, kSinkPort).empty(), "confirmation stops fast sends");
    // The real fast timer above proves callback wiring. Drive steady callbacks
    // directly so runner load cannot turn a delayed timeout into a failure.
    const int steadyTimerId = WanUdpKeepaliveTestAccess::timer(stream).timerId();
    for (int i = 0; i < 2; ++i) {
        WanUdpKeepaliveTestAccess::timeout(stream);
        packets = drain(recorded, kSinkPort);
        check(packets.size() == 1 && allEqual(packets, expected),
              "steady callback keeps sending the registration payload");
        WanUdpKeepaliveTestAccess::feed(stream, valid);
        check(WanUdpKeepaliveTestAccess::timer(stream).timerId() == steadyTimerId
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "continued receive does not restart the five-second timer");
    }

    // FlexLib's Opus writer rounds the VITA word count up but emits only
    // the actual byte payload. One to three bytes of padding may be absent.
    for (int payloadBytes = 1; payloadBytes <= 4; ++payloadBytes) {
        QByteArray opus = valid.left(28) + QByteArray(payloadBytes, '\0');
        auto* raw = reinterpret_cast<uchar*>(opus.data());
        qToBigEndian<quint32>(0x38D00000u | quint32((opus.size() + 3) / 4), raw);
        qToBigEndian<quint32>(0x534C8005u, raw + 12);
        stream.startWanUdpRegister(0xAu);
        WanUdpKeepaliveTestAccess::feed(stream, opus);
        check(WanUdpKeepaliveTestAccess::confirmed(stream)
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "Opus with a rounded word count qualifies without transmitted padding");
    }
    pump(10);
    drain(recorded, kSinkPort);

    // Qualification follows the preamble, independently of stream dispatch:
    // data-with-stream types and optional timestamps are legal.
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
    // FlexLib qualifies the parsed preamble before payload dispatch. Packet
    // size and trailer fields do not participate, and non-stream types place
    // their class immediately after the first header word.
    QByteArray mismatchedSize = valid;
    mismatchedSize[2] = 0;
    mismatchedSize[3] = 0;
    preambles.push_back(mismatchedSize);
    QByteArray trailerOnlyFlag = noTimestamps;
    trailerOnlyFlag[0] = char(0x3C);
    preambles.push_back(trailerOnlyFlag);
    for (quint32 type : {0u, 2u, 4u, 5u}) {
        QByteArray noStream(16, '\0');
        auto* raw = reinterpret_cast<uchar*>(noStream.data());
        qToBigEndian<quint32>((type << 28) | 0x08000004u, raw);
        qToBigEndian<quint32>(0x001C2Du, raw + 4);
        qToBigEndian<quint32>(0x534C8002u, raw + 8);
        preambles.push_back(noStream);
    }
    for (const QByteArray& preamble : preambles) {
        stream.startWanUdpRegister(0xAu);
        WanUdpKeepaliveTestAccess::feed(stream, preamble);
        check(WanUdpKeepaliveTestAccess::confirmed(stream)
              && WanUdpKeepaliveTestAccess::timer(stream).interval() == 5000,
              "valid alternative Flex preamble qualifies before stream dispatch");
    }
    pump(10);
    drain(recorded, kSinkPort);

    stream.startWanUdpRegister(0xABCDEF01u);
    pump(120);
    packets = drain(recorded, kSinkPort);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0xABCDEF01"))
          && !WanUdpKeepaliveTestAccess::confirmed(stream), "handle replacement restarts fast cadence without old payloads");
    stream.stop();
    drain(recorded, kSinkPort);
    const qint64 stoppedBytes = stream.totalTxBytes();
    WanUdpKeepaliveTestAccess::timeout(stream);
    stream.startWanUdpRegister(0xAu);
    pump(120);
    check(drain(recorded, kSinkPort).empty() && stream.totalTxBytes() == stoppedBytes
          && !WanUdpKeepaliveTestAccess::timer(stream).isActive(), "stop fences late timeouts and late handle delivery");

    check(stream.startWan(QHostAddress::LocalHost, kReplacementPort), "reconnect selects the replacement destination");
    pump(100);
    check(drain(recorded, kReplacementPort).empty(), "reconnect cannot reuse the previous handle");
    stream.startWanUdpRegister(0xBu);
    pump(120);
    packets = drain(recorded, kReplacementPort);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0x0000000B"))
          && drain(recorded, kSinkPort).empty(), "reconnect sends only new handle to new destination");
    WanUdpKeepaliveTestAccess::feed(stream, wrongOui);
    check(!WanUdpKeepaliveTestAccess::confirmed(stream), "non-Flex traffic cannot confirm the new session");

    // Replacing a running WAN destination clears the handle before any send.
    check(stream.startWan(QHostAddress::LocalHost, kSinkPort), "active WAN destination can be replaced");
    pump(110);
    check(drain(recorded, kSinkPort).empty() && drain(recorded, kReplacementPort).empty(), "destination replacement retires old timer and handle");
    stream.startWanUdpRegister(0xCu);
    pump(110);
    packets = drain(recorded, kSinkPort);
    check(packets.size() >= 2 && allEqual(packets, QByteArray("client udp_register handle=0x0000000C")),
          "replacement destination uses only the replacement handle");

    writeSucceeds = false;
    const qint64 bytesBeforeFailure = stream.totalTxBytes();
    WanUdpKeepaliveTestAccess::timeout(stream);
    check(stream.totalTxBytes() == bytesBeforeFailure && recorded.empty(),
          "failed datagram writes do not count bytes");
    writeSucceeds = true;
    stream.stop();
    bindSucceeds = false;
    check(!stream.startWan(QHostAddress::LocalHost, kSinkPort), "failed bind reports failure");
    stream.startWanUdpRegister(0xDu);
    WanUdpKeepaliveTestAccess::timeout(stream);
    check(recorded.empty() && !stream.isRunning()
          && !WanUdpKeepaliveTestAccess::timer(stream).isActive(),
          "failed reconnect cannot send with the old destination or handle");
    check(WanUdpKeepaliveTestAccess::nativeSocketUnopened(stream),
          "all production WAN behavior ran without opening a native UDP socket");
    stream.stop();
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
