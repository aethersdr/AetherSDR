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

    // ---- S3 row 3.4: an ESTABLISHED link that goes quiet re-starts itself ----
    //
    // The failure this recovers from is the one the client used to have no
    // answer to at all. dsopenhpsdr1.v's anti-wedge watchdog is cleared by EP2
    // ARRIVALS and eventually sets `run <= 0`, so the radio stops streaming on
    // its own, without being asked and without any stop from us. Before this,
    // onWatchdogTick() cleared m_linkUp, emitted linkDown and could then never
    // fire again -- m_startRetryTimer was armed by start() and setReceiverCount()
    // and by nothing else -- so resuming EP2 could not restart the radio and the
    // only recovery was RadioModel tearing the whole backend down five seconds
    // later and rebuilding every WDSP channel.
    //
    // The fake radio models exactly that: `streaming` goes false with no stop
    // datagram, and only a run command brings it back.
    //
    // NEGATIVE CONTROL FIRST. Everything above this point was a healthy session
    // plus two deliberate restarts. If the silence recovery can be provoked by
    // any of that, the positive result below is worthless.
    check(client.silenceRecoveryAttempts() == 0,
          "NEGATIVE CONTROL: a healthy session and two clean restarts trip no silence recovery");
    check(client.silenceRecoveriesCompleted() == 0,
          "NEGATIVE CONTROL: and complete none");

    {
        const int startsBeforeSilence = startsSeen;
        const int stopsBeforeSilence  = stopsSeen;
        const int upsBefore   = upSpy.count();
        const int downsBefore = downSpy.count();

        // The radio wedges: it stops streaming and never says so.
        streaming = false;
        blocksSeen = 0;
        spin(1500);   // inside kSilenceTimeoutMs (2000) -- nothing should happen yet
        check(client.silenceRecoveryAttempts() == 0,
              "no recovery before the silence timeout expires");
        check(blocksSeen == 0, "and no EP6, because the radio really has stopped");

        spin(1500);   // now past 2000 ms of silence, plus room for the run command
        check(client.silenceRecoveryAttempts() == 1,
              "the silence watchdog re-sent the run command instead of declaring link loss");
        check(startsSeen == startsBeforeSilence + 1,
              "exactly one run command went out for the silence");
        check(stopsSeen == stopsBeforeSilence,
              "and NO metis-stop -- the payload layout did not change, so there is "
              "no hard edge to make and nothing to re-prime");
        check(blocksSeen > 0, "EP6 resumed: the radio was restarted by the run command");
        check(client.silenceRecoveriesCompleted() == 1,
              "and the recovery is recorded as completed, not merely attempted");

        // A recovery that worked must be INVISIBLE above the protocol layer.
        // Hl2Backend republishes its entire initial state on linkUp, over the
        // operator's live panes, and RadioModel starts a five-second teardown on
        // the disconnected() that follows linkDown. Emitting either here would
        // have cost more than the fault did.
        check(downSpy.count() == downsBefore, "a recovered silence emits no linkDown");
        check(upSpy.count() == upsBefore, "and no linkUp, so nothing republishes");
    }

    {
        // ---- and a link that is genuinely gone is still declared gone ----
        //
        // The recovery must not become a way of never reporting link loss. With
        // every run command ignored, the retry budget runs out and the watchdog
        // falls through to exactly the teardown it always did.
        const int downsBefore = downSpy.count();
        startsToDrop = 99;      // the radio ignores every start from here on
        streaming = false;
        spin(5000);             // 2000 silence + 1500 retry budget + margin
        check(client.silenceRecoveryAttempts() == 2, "a second silence gets its own recovery");
        check(client.silenceRecoveriesCompleted() == 1,
              "which does NOT complete, because the radio never came back");
        check(downSpy.count() == downsBefore + 1,
              "an unrecoverable silence still reports link loss");
    }

    client.stop();

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_receiver_count_restart_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
