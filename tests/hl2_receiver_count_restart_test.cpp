// HL2 — the receiver-count restart must survive a LOST metis-start.
//
// setReceiverCount() stops the EP6 stream, rebuilds the payload layout and starts
// it again. That start is a single UDP datagram and is exactly as losable as the
// one at connect, so it needs the same retry. Without it, one dropped packet left
// the radio silent with nothing re-asking it to stream: kSilenceTimeoutMs later
// the EP6 watchdog reported link loss and the operator's session died — from
// having clicked "Add Panadapter".
//
// The fake radio here models a radio that IGNORES one start and honours the next,
// which is what a dropped datagram looks like from the host side. It also GATES
// EP6 on its own start/stop state: the whole assertion is that samples stop and
// then come back, so a fixture that streamed regardless would pass with or
// without the retry and prove nothing.
//
// The second half is the reason this is not just a timer test. A restart must not
// look like a reconnect — Hl2Backend republishes its entire initial state on
// linkUp, over the operator's live panes — so linkUp must fire exactly once
// across the whole run and linkDown not at all.

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

// A minimal valid EP6 packet: header plus both frame SYNCs. The samples are zero
// because what is asserted here is the block GEOMETRY — how many receivers the
// round decodes into — not any sample value.
static QByteArray fakeEp6(std::uint32_t seq)
{
    QByteArray p(static_cast<int>(kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;                                         // frame A SYNC
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

    // ---- a fake HL2 that honours start/stop, and can "lose" a start ----
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();

    bool streaming = false;
    int startsSeen = 0;
    int stopsSeen = 0;
    int startsToDrop = 0;   // pretend this many metis-start datagrams never arrived
    std::uint32_t nextEp6Seq = 0;
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            const QByteArray& d = dg.data();
            // EF FE 04 <run>: bit 0 is start/stop (bit 7 is the watchdog-disable
            // flag, which MetisClient leaves clear).
            if (d.size() >= 4 && static_cast<std::uint8_t>(d[2]) == 0x04) {
                if (static_cast<std::uint8_t>(d[3]) & 0x01) {
                    ++startsSeen;
                    if (startsToDrop > 0)
                        --startsToDrop;    // "lost in the network" — no state change
                    else
                        streaming = true;
                } else {
                    ++stopsSeen;
                    streaming = false;
                }
                continue;                  // a command is not answered with IQ
            }
            // C&C (EP2). A started radio answers each one with an EP6 packet,
            // which is what keeps the ping-pong going; a stopped one says nothing.
            if (streaming)
                radio.writeDatagram(fakeEp6(nextEp6Seq++), dg.senderAddress(), dg.senderPort());
        }
    });

    // ---- MetisClient against it ----
    MetisClient client;
    QSignalSpy upSpy(&client, &MetisClient::linkUp);
    QSignalSpy downSpy(&client, &MetisClient::linkDown);
    int blocksSeen = 0;
    int lastBlockCount = 0;
    QObject::connect(&client, &MetisClient::iqBlocksReady, &client,
                     [&](const std::vector<std::vector<std::complex<float>>>& blocks) {
                         ++blocksSeen;
                         lastBlockCount = static_cast<int>(blocks.size());
                     });

    MetisClient::Params p;
    p.host = QHostAddress::LocalHost;
    p.port = radioPort;
    p.rxFrequencyHz = 7'100'000;
    p.numRx = 1;
    p.boardMaxRx = 4;
    check(client.start(p), "client starts");
    spin(300);
    check(upSpy.count() == 1, "linkUp on the first EP6");
    check(blocksSeen > 0, "EP6 flowing with one receiver");
    check(lastBlockCount == 1, "payload decodes as one receiver to begin with");

    // ---- the restart's metis-start goes missing ----
    startsToDrop = 1;
    const int startsBefore = startsSeen;
    const int stopsBefore = stopsSeen;
    client.setReceiverCount(2);

    // SETTLE BEFORE ASSERTING ANYTHING. setReceiverCount blocks in
    // sendPrimingBurst's msleeps with no event loop running, so at the instant it
    // returns the fake radio has not yet seen the stop or the start — they are
    // sitting in its socket, and so are the stragglers it sent before them. Both
    // ends of this test live in one event loop; a real radio and a real host do
    // not, which is exactly the asymmetry the retry has to survive.
    spin(60);
    check(stopsSeen == stopsBefore + 1, "the restart stopped the stream first");
    check(startsSeen == startsBefore + 1, "and sent one start, which the radio lost");
    check(!streaming, "the radio is stopped — the start it lost never started it");

    // Measure the gap from a clean slate: the claim is that nothing NEW arrives
    // while the lost start leaves the radio stopped, not that the socket was empty
    // when the stop went out.
    blocksSeen = 0;
    spin(120);   // still short of kStartRetryMs (300)
    check(blocksSeen == 0, "no EP6 while the lost start leaves the radio stopped");

    // ---- the retry re-sends it, and the stream comes back in the new layout ----
    spin(700);
    check(startsSeen >= startsBefore + 2, "the retry re-sent the restart's metis-start");
    check(blocksSeen > 0, "EP6 resumed after the retry");
    check(lastBlockCount == 2, "and resumed in the TWO-receiver layout");

    // A receiver-count change is not a reconnect. Hl2Backend republishes its whole
    // initial state on linkUp, so a spurious one here would wipe and rebuild the
    // operator's panes in the middle of adding a panadapter.
    check(upSpy.count() == 1, "no spurious linkUp across the restart");
    check(downSpy.count() == 0, "no linkDown across the restart");

    // ---- and a restart whose start is NOT lost needs no retry at all ----
    const int startsBeforeClean = startsSeen;
    blocksSeen = 0;
    client.setReceiverCount(3);
    spin(400);   // past one kStartRetryMs, so a stuck retry would have fired
    check(blocksSeen > 0, "EP6 flowing again after a clean restart");
    check(lastBlockCount == 3, "payload decodes as three receivers");
    check(startsSeen == startsBeforeClean + 1,
          "a start that landed is not re-sent — the arriving EP6 disarms the retry");
    check(upSpy.count() == 1, "still exactly one linkUp for the whole session");
    check(downSpy.count() == 0, "still no linkDown");

    client.stop();

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_receiver_count_restart_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
