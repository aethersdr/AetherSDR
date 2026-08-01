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
    check(lastBlockSize == static_cast<std::size_t>(kEp6BlockSamples),
          "decoded block carries 126 IQ samples");
    check(client.droppedPackets() == 0, "no drops on an ordered loopback stream");

    // ---- transport counters (the network readouts' only source on this family) ----
    //
    // Published on a ~1 s window, so this spins past one boundary rather than
    // reading the accessor: the SIGNAL is what Hl2Backend mirrors, and a counter
    // that increments without ever being published reaches no readout.
    QSignalSpy linkSpy(&client, &MetisClient::linkCountersUpdated);
    spin(1200);
    check(linkSpy.count() >= 1, "linkCountersUpdated published on its window");
    // By value: the accessor is I/O-thread-only and hands out a copy, because a
    // reference to a struct holding a QString is a refcount race, not a read.
    const MetisClient::LinkCounters lc = client.linkCounters();
    check(lc.rxPackets > 0, "EP6 packets counted");
    check(lc.rxBytes >= lc.rxPackets * kUsbPacketSize, "received bytes counted");
    // EP2 is paced off a wall clock, so it flows regardless of what comes back.
    check(lc.txPackets > 0 && lc.txBytes > 0, "sent datagrams counted");
    check(lc.drops == 0, "ordered loopback stream loses nothing");
    check(!lc.localEndpoint.isEmpty(), "bound local endpoint captured");
    // The socket is bound wildcard, so localAddress() is 0.0.0.0 and naming it
    // would tell an operator nothing about which interface reaches the radio.
    // The kernel knows, per datagram, and ReceivePacketDestinationAddress is how
    // we ask — on loopback that resolves to 127.0.0.1. If a platform refuses the
    // option the endpoint stays "*:<port>", which is at least not a claim.
    check(!lc.localEndpoint.startsWith(QStringLiteral("0.0.0.0")),
          "local endpoint is not the wildcard address a bare bind reports");
    check(lc.localEndpoint.startsWith(QStringLiteral("127.0.0.1:")),
          "local endpoint resolves to the interface the datagrams arrived on");
    // Published AFTER the drain that carries them: a snapshot emitted before the
    // wakeup's own datagrams were counted is one full drain out of date.
    const auto& published = qvariant_cast<MetisClient::LinkCounters>(
        linkSpy.at(linkSpy.count() - 1).at(0));
    check(published.rxPackets > 0,
          "the published snapshot carries the packets of its own drain");
    // Timed once per socket wakeup, not once per datagram: successive datagrams
    // inside one drain are microseconds apart no matter how the link behaves, so
    // per-datagram timing would report a steady 0 ms straight through a stall.
    check(lc.meanGapMs >= 0 && lc.maxGapMs >= lc.meanGapMs,
          "inter-arrival gap measured, maximum not below the mean");

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
