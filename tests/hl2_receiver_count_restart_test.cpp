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
    // ---- two ways a radio can come back, and the client must tell them apart ----
    //
    // The silence-recovery path deliberately does NOT reset m_haveRxSeq, and the
    // 60-line comment defending that rests on both of these being handled by the
    // same do-nothing. They are mutually exclusive, so they get a silence each.
    //
    // THE RADIO NEVER HALTED and the silence was ours: it kept counting while we
    // heard nothing, so the jump across the gap is a REAL loss of that many
    // packets and must be scored. Applied to the start that ends the silence.
    std::uint32_t seqBumpOnStart = 0;
    // THE RADIO HALTED and this start restarts it: the gateware zeroes ep6_seq_no
    // on ~run, so the sequence begins again at zero and the backward jump is a
    // RESET, not a loss. Nothing may be scored.
    bool restartSeqOnStart = false;
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            const QByteArray& d = dg.data();
            // EF FE 04 <run>: bit 0 is start/stop (bit 7 is the watchdog-disable
            // flag, which MetisClient leaves clear).
            if (d.size() >= 4 && static_cast<std::uint8_t>(d[2]) == 0x04) {
                if (static_cast<std::uint8_t>(d[3]) & 0x01) {
                    ++startsSeen;
                    if (startsToDrop > 0) {
                        --startsToDrop;    // "lost in the network" — no state change
                    } else {
                        streaming = true;
                        if (restartSeqOnStart) {
                            restartSeqOnStart = false;
                            nextEp6Seq = 0;          // ~run zeroed ep6_seq_no
                        }
                        nextEp6Seq += seqBumpOnStart; // packets we never received
                        seqBumpOnStart = 0;
                    }
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

    // ---- an ESTABLISHED link that goes quiet re-starts itself ----
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
    // THIS GROWS A LEGACY SOCKET FIXTURE, KNOWINGLY. AGENTS.md routes a
    // disconnected input to a socket-free test that injects the transport, and
    // names this target's fake Metis radio as one of four legacy exceptions
    // tracked for extraction in #5254. The reason the silence path lands here
    // anyway is mechanical: stage 1's only observable is a datagram sent
    // through MetisClient's own socket, and there is no injectable datagram
    // sink beside feedDatagram() to watch it with. When #5254 adds one, these
    // three blocks are the first thing that should move behind it -- the fake's
    // run-gating and its sequence knobs are exactly the input data that
    // extraction wants.
    //
    // NEGATIVE CONTROL FIRST. Everything above this point was a healthy session
    // plus two deliberate restarts. If the silence recovery can be provoked by
    // any of that, the positive result below is worthless.
    check(client.linkCounters().silenceRecoveryAttempts == 0,
          "NEGATIVE CONTROL: a healthy session and two clean restarts trip no silence recovery");
    check(client.linkCounters().silenceRecoveriesCompleted == 0,
          "NEGATIVE CONTROL: and complete none");

    {
        const int startsBeforeSilence = startsSeen;
        const int stopsBeforeSilence  = stopsSeen;
        const int upsBefore   = upSpy.count();
        const int downsBefore = downSpy.count();
        const quint64 dropsBefore = client.droppedPackets();

        // AND THE RADIO KEPT COUNTING THROUGH IT. This is the half of the
        // sequence argument where doing nothing has to SCORE something: the
        // client's silence was its own, the stream never stopped advancing
        // ep6_seq_no, and the jump waiting on the other side of the gap is a
        // genuine loss. Resetting m_haveRxSeq here would erase it silently.
        const std::uint32_t lostAcrossTheSilence = 761;
        seqBumpOnStart = lostAcrossTheSilence;

        // The radio wedges: it stops streaming and never says so.
        streaming = false;
        blocksSeen = 0;
        spin(1500);   // inside kSilenceTimeoutMs (2000) -- nothing should happen yet
        check(client.linkCounters().silenceRecoveryAttempts == 0,
              "no recovery before the silence timeout expires");
        check(blocksSeen == 0, "and no EP6, because the radio really has stopped");

        spin(1500);   // now past 2000 ms of silence, plus room for the run command
        check(client.linkCounters().silenceRecoveryAttempts == 1,
              "the silence watchdog re-sent the run command instead of declaring link loss");
        check(startsSeen == startsBeforeSilence + 1,
              "exactly one run command went out for the silence");
        check(stopsSeen == stopsBeforeSilence,
              "and NO metis-stop -- the payload layout did not change, so there is "
              "no hard edge to make and nothing to re-prime");
        check(blocksSeen > 0, "EP6 resumed: the radio was restarted by the run command");
        check(client.linkCounters().silenceRecoveriesCompleted == 1,
              "and the recovery is recorded as completed, not merely attempted");
        check(client.droppedPackets() == dropsBefore + lostAcrossTheSilence,
              "a real forward gap across the silence is still counted as loss -- "
              "the recovery does not reset sequence tracking");

        // A recovery that worked must be INVISIBLE above the protocol layer.
        // Hl2Backend republishes its entire initial state on linkUp, over the
        // operator's live panes, and RadioModel starts a five-second teardown on
        // the disconnected() that follows linkDown. Emitting either here would
        // have cost more than the fault did.
        check(downSpy.count() == downsBefore, "a recovered silence emits no linkDown");
        check(upSpy.count() == upsBefore, "and no linkUp, so nothing republishes");
    }

    {
        // ---- the OTHER half of the sequence argument: the radio really halted ----
        //
        // The gateware zeroes ep6_seq_no on ~run, so a radio that was stopped and
        // is restarted by this run command begins again at zero. That backward
        // jump is a RESET and not a loss, and the recovery leans on the existing
        // guard (`gap < 0x80000000u`) declining to score it rather than on
        // resetting m_haveRxSeq itself. If that guard ever stops covering this,
        // a recovered silence starts reporting about four billion dropped
        // packets on the operator's health row.
        const quint64 dropsBefore = client.droppedPackets();
        const int downsBefore = downSpy.count();
        const int upsBefore   = upSpy.count();
        restartSeqOnStart = true;
        streaming = false;
        blocksSeen = 0;
        spin(3000);             // 2000 silence + the run command and its answer
        check(client.linkCounters().silenceRecoveryAttempts == 2,
              "a second silence gets its own recovery");
        check(client.linkCounters().silenceRecoveriesCompleted == 2,
              "and this one completes too");
        check(blocksSeen > 0, "EP6 resumed, counting from zero again");
        check(client.droppedPackets() == dropsBefore,
              "a restarted sequence is a RESET, not four billion dropped packets");
        check(downSpy.count() == downsBefore, "still no linkDown");
        check(upSpy.count() == upsBefore, "still no linkUp");
    }

    {
        // ---- and a link that is genuinely gone is still declared gone ----
        //
        // The recovery must not become a way of never reporting link loss. With
        // every run command ignored, the retry budget runs out and the watchdog
        // falls through to exactly the teardown it always did.
        const int downsBefore = downSpy.count();
        const int startsBefore = startsSeen;
        startsToDrop = 99;      // the radio ignores every start from here on
        streaming = false;
        // The failed attempt has to leave the I/O thread without an EP6 wakeup.
        // onReadyRead is what normally emits linkCountersUpdated, and it does
        // not run when every run command is ignored. Drop the emit in stage 1
        // and this spy stays empty while the getter above still reads 3.
        QSignalSpy published(&client, &MetisClient::linkCountersUpdated);
        spin(5000);             // 2000 silence + 1500 retry budget + margin
        check(client.linkCounters().silenceRecoveryAttempts == 3,
              "a third silence gets its own recovery");
        check(client.linkCounters().silenceRecoveriesCompleted == 2,
              "which does NOT complete, because the radio never came back");
        bool publishedFailure = false;
        for (int i = 0; i < published.count(); ++i) {
            const auto row = published.at(i).at(0).value<MetisClient::LinkCounters>();
            if (row.silenceRecoveryAttempts == 3 && row.silenceRecoveriesCompleted == 2)
                publishedFailure = true;
        }
        check(publishedFailure,
              "a failed recovery is published on linkCountersUpdated with no EP6 wakeup");
        // THE RUN COMMANDS REALLY WENT OUT, and there were the right number of
        // them. kMaxStartAttempts is 5 and armStartRetry() seeds the count at 1
        // to charge for the datagram stage 1 just sent, so the wire sees that
        // one plus four re-sends at kStartRetryMs before the budget is spent.
        // Without this the failure block could pass on a recovery that armed a
        // counter and never touched the socket.
        check(startsSeen == startsBefore + 5,
              "five run commands went out for the unrecoverable silence -- one from "
              "stage 1 and four from the retry -- and then the budget was spent");
        check(downSpy.count() == downsBefore + 1,
              "an unrecoverable silence still reports link loss");
    }

    client.stop();

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_receiver_count_restart_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
