// aetherd HL2 Phase 1a — MetisClient loopback test. A fake HL2 on localhost
// replies to each datagram with an incrementing EP6 packet; verifies MetisClient
// sends the start command, ingests EP6 into 126-sample IQ blocks, emits linkUp,
// paces C&C 1:1, tracks drops, and survives live control + stop. No hardware.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>

#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// A minimal valid EP6 packet with the given sequence (header + both frame SYNCs;
// samples default to zero — the decode count is what we assert here).
static QByteArray fakeEp6(std::uint32_t seq)
{
    QByteArray p(static_cast<int>(kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;                                     // frame A SYNC
    b[8 + kFrameSize] = b[9 + kFrameSize] = b[10 + kFrameSize] = 0x7F;  // frame B SYNC
    return p;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- fake HL2: reply to every datagram with the next EP6 ----
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();
    std::uint32_t nextEp6Seq = 0;
    int startsSeen = 0;
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            const QByteArray& d = dg.data();
            if (d.size() >= 4 && static_cast<std::uint8_t>(d[2]) == 0x04
                && static_cast<std::uint8_t>(d[3]) == 0x01) {
                ++startsSeen;                                       // metis start command
            }
            const QByteArray ep6 = fakeEp6(nextEp6Seq++);
            radio.writeDatagram(ep6, dg.senderAddress(), dg.senderPort());
        }
    });

    // ---- MetisClient against the fake radio ----
    MetisClient client;
    QSignalSpy upSpy(&client, &MetisClient::linkUp);
    int iqCount = 0;
    std::size_t lastBlockSize = 0;
    QObject::connect(&client, &MetisClient::iqBlockReady, &client,
                     [&](const std::vector<std::complex<float>>& block) {
                         ++iqCount;
                         lastBlockSize = block.size();
                     });

    MetisClient::Params p;
    p.host = QHostAddress::LocalHost;
    p.port = radioPort;
    p.rxFrequencyHz = 7'100'000;
    check(client.start(p), "client starts");
    check(client.isRunning(), "client reports running");

    spin(300);   // let the EP6 <-> C&C ping-pong flow

    check(startsSeen >= 1, "fake radio saw the metis start command");
    check(upSpy.count() == 1, "linkUp emitted exactly once (first EP6)");
    check(iqCount >= 5, "iqBlockReady fired for the EP6 stream");
    check(lastBlockSize == static_cast<std::size_t>(kSamplesPerPacket),
          "decoded block carries 126 IQ samples");
    check(client.droppedPackets() == 0, "no drops on an ordered loopback stream");

    // ---- live control does not disrupt the stream ----
    const int before = iqCount;
    client.setRxFrequencyHz(14'100'000);
    client.setLnaGainDb(30);
    spin(80);
    check(client.isRunning() && iqCount > before, "stream continues after live control");

    // ---- stop is clean ----
    QSignalSpy downSpy(&client, &MetisClient::linkDown);
    client.stop();
    check(!client.isRunning(), "client stops");
    check(downSpy.count() == 1, "linkDown emitted on stop");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_metis_client_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
