// HL2 — the receiver-count restart must not stall the thread that feeds the radio.
//
// setReceiverCount() stops EP6, rebuilds the payload layout, primes the new
// config and starts EP6 again. The priming banks are spaced 10 ms apart, and that
// spacing used to be two QThread::msleep(10) per sendPrimingBurst() call, two
// calls per restart: ~40 ms in which MetisClient's thread -- the one that paces
// EP2 and drains EP6 -- ran no event loop at all (#5678 row 3.5). It is now
// driven by a timer on that thread. This test pins what that must and must not
// change.
//
// WHAT MUST NOT CHANGE, asserted on what the client puts on the wire:
//   - the order: metis-stop, C&C carrying the NEW receiver count, metis-start;
//   - the spacing: the start goes out >= 20 ms after the stop (two 10 ms banks),
//     and the post-start banks and the bandscope re-arm >= 20 ms after that;
//   - every C&C after the stop already carries the new count;
//   - no run byte of any kind between the stop and the start -- every run byte
//     has bit 0 set, so a bandscope arm inside the window is an early start;
//   - EP6 arriving between stop and start (old layout) is never decoded.
// WHAT MUST CHANGE:
//   - setReceiverCount() returns without waiting out the spacing;
//   - EP2 keeps flowing through the restart: no C&C gap anywhere near 10 ms.
//
// SOCKET-FREE, per AGENTS.md's test-layer table: this is scheduling policy, so
// the transport is injected. C&C frames go to m_packetSinkForTest, run/stop
// datagrams to m_commandSinkForTest, and "arriving" EP6 is handed to
// handleDatagram() directly. Nothing binds, nothing is sent, no peer exists.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // Streaming, with the EP2 pacer running, against an injected transport.
    static void startInjected(
        MetisClient& c,
        std::function<qint64(const std::array<std::uint8_t, kUsbPacketSize>&)> packets,
        std::function<qint64(const std::array<std::uint8_t, 64>&)> commands)
    {
        c.m_packetSinkForTest = std::move(packets);
        c.m_commandSinkForTest = std::move(commands);
        c.m_running = true;
        c.m_ep2IntervalUs = static_cast<qint64>(kTxSamplesPerPacket) * 1'000'000
                          / MetisClient::kEp2AudioRateHz;
        c.m_ep2Sent = 0;
        c.m_ep2Clock.restart();
        c.m_ep2Timer->start();
    }
    static void feedDatagram(MetisClient& c, std::span<const std::uint8_t> bytes)
    {
        c.handleDatagram(bytes);
    }
    static int stalePackets(const MetisClient& c) { return c.m_restartStalePackets; }
    static bool restartIdle(const MetisClient& c)
    {
        return c.m_restartStep == MetisClient::RestartStep::Idle;
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static std::vector<std::uint8_t> fakeEp6(std::uint32_t seq)
{
    std::vector<std::uint8_t> b(kUsbPacketSize, 0);
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;
    b[8 + kFrameSize] = b[9 + kFrameSize] = b[10 + kFrameSize] = 0x7F;
    return b;
}

// One thing the client put on the wire.
struct Ev {
    double tMs = 0;
    char kind = '?';          // 'S' run byte with bit 0 clear, 'R' bit 0 set, 'C' C&C
    std::uint8_t run = 0;     // the run byte, for 'S' / 'R'
    int numRx = 0;            // bank A's receiver count, for 'C'
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QElapsedTimer clock;
    clock.start();
    auto nowMs = [&clock] { return clock.nsecsElapsed() / 1e6; };
    std::vector<Ev> ev;

    MetisClient client;
    int lastBlockCount = 0;
    QObject::connect(&client, &MetisClient::iqBlocksReady, &client,
                     [&](const std::vector<std::vector<std::complex<float>>>& blocks) {
                         lastBlockCount = static_cast<int>(blocks.size());
                     });
    MetisClientTestAccess::startInjected(
        client,
        [&](const std::array<std::uint8_t, kUsbPacketSize>& pkt) -> qint64 {
            Ev e;
            e.tMs = nowMs();
            e.kind = 'C';
            // Frame A: 7F 7F 7F C0 C1 C2 C3 C4 at offset 8. Bank A is the config
            // register on every frame; C4[6:3] = numRx - 1.
            e.numRx = ((pkt[15] >> 3) & 0x0F) + 1;
            ev.push_back(e);
            return static_cast<qint64>(pkt.size());
        },
        [&](const std::array<std::uint8_t, 64>& cmd) -> qint64 {
            Ev e;
            e.tMs = nowMs();
            e.run = cmd[3];
            e.kind = (cmd[3] & 0x01) ? 'R' : 'S';
            ev.push_back(e);
            return static_cast<qint64>(cmd.size());
        });

    std::uint32_t seq = 0;
    spin(50);
    check(!ev.empty() && ev.back().kind == 'C' && ev.back().numRx == 1,
          "EP2 pacer running with one receiver before the restart");

    // ================= 1: one restart, with EP6 stragglers and a bandscope enable
    // Old-layout EP6 "arriving" inside the window, at +5 and +12 ms, on the
    // client's own event loop -- which only turns there now that it is not
    // asleep. Six in all.
    int stragglersFed = 0;
    for (int at : {5, 12}) {
        QTimer::singleShot(at, &client, [&] {
            for (int i = 0; i < 3; ++i) {
                const auto p = fakeEp6(seq++);
                MetisClientTestAccess::feedDatagram(client, p);
                ++stragglersFed;
            }
        });
    }
    const double callAt = nowMs();
    QElapsedTimer callTimer;
    callTimer.start();
    client.setReceiverCount(2);
    const qint64 callNs = callTimer.nsecsElapsed();
    // Inside the stop-to-start window: this arms the gate at once, which would
    // put a 0x03 run byte -- an early start -- on the wire if nothing held it.
    client.setBandscopeEnabled(true);
    std::fprintf(stderr, "     setReceiverCount() returned in %.3f ms\n", callNs / 1e6);
    check(callNs < 10'000'000, "setReceiverCount() returns without waiting out the 10 ms banks");
    check(!MetisClientTestAccess::restartIdle(client),
          "the restart is still in flight when the call returns");

    spin(80);

    int iStop = -1, iStart = -1;
    for (int i = 0; i < static_cast<int>(ev.size()); ++i) {
        if (ev[i].tMs < callAt) continue;
        if (iStop < 0 && ev[i].kind == 'S') iStop = i;
        else if (iStop >= 0 && ev[i].kind == 'R') { iStart = i; break; }
    }
    check(iStop >= 0 && iStart > iStop, "a stop, then a run byte, went out");
    if (iStop >= 0 && iStart > iStop) {
        const double stopT = ev[iStop].tMs;
        const double startT = ev[iStart].tMs;
        check(ev[iStart].run == 0x01,
              "the first run byte after the stop is the plain metis-start (0x01)");
        std::fprintf(stderr, "     start went out %.2f ms after the stop\n", startT - stopT);
        check(startT - stopT >= 20.0, "the start follows the stop by >= 20 ms (two banks)");

        int ccInWindow = 0, ccOldCount = 0;
        for (int i = iStop + 1; i < iStart; ++i) {
            if (ev[i].kind == 'C') {
                ++ccInWindow;
                if (ev[i].numRx != 2)
                    ++ccOldCount;
            }
        }
        std::fprintf(stderr, "     %d C&C frames between stop and start\n", ccInWindow);
        check(ccInWindow >= 6, ">= 6 C&C frames primed the radio before the start");
        check(ccOldCount == 0, "every C&C after the stop carries the NEW receiver count");

        // EP2 CONTINUITY: from the last C&C before the stop to 20 ms past the
        // start. The blocking form put >= 10 ms between banks with the pacer
        // stopped; the pacer's own cadence is 2.625 ms.
        double prevCc = -1, maxGap = 0;
        for (const Ev& e : ev) {
            if (e.kind != 'C' || e.tMs > startT + 20.0)
                continue;
            if (e.tMs < stopT) { prevCc = e.tMs; continue; }
            if (prevCc >= 0 && e.tMs - prevCc > maxGap)
                maxGap = e.tMs - prevCc;
            prevCc = e.tMs;
        }
        std::fprintf(stderr, "     largest C&C gap across the restart: %.2f ms\n", maxGap);
        check(maxGap < 8.0, "EP2 keeps flowing through the restart (no C&C gap near 10 ms)");

        int ccAfterStart = 0;
        for (const Ev& e : ev)
            if (e.kind == 'C' && e.tMs >= startT && e.tMs <= startT + 20.0)
                ++ccAfterStart;
        check(ccAfterStart >= 6, ">= 6 C&C frames in the 20 ms after the start");

        int iGate = -1;
        for (int i = iStart + 1; i < static_cast<int>(ev.size()); ++i)
            if (ev[i].kind == 'R' && ev[i].run == 0x03) { iGate = i; break; }
        check(iGate >= 0, "the bandscope enabled inside the window is re-armed after it");
        if (iGate >= 0) {
            std::fprintf(stderr, "     bandscope run byte went out %.2f ms after the start\n",
                         ev[iGate].tMs - startT);
            check(ev[iGate].tMs - startT >= 20.0,
                  "and only after the two post-start banks (>= 20 ms)");
        }
    }
    std::fprintf(stderr, "     stragglers fed %d, discarded undecoded %d\n",
                 stragglersFed, MetisClientTestAccess::stalePackets(client));
    check(stragglersFed == 6, "six old-layout EP6 packets arrived inside the window");
    check(MetisClientTestAccess::stalePackets(client) == stragglersFed,
          "every one was discarded, none decoded against the new layout");
    check(lastBlockCount == 0, "no IQ block was published from the old layout");
    check(MetisClientTestAccess::restartIdle(client), "the restart completed");

    // After the start, EP6 decodes again, in the new layout.
    for (std::uint32_t i = 0; i < 8; ++i) {
        const auto p = fakeEp6(i);
        MetisClientTestAccess::feedDatagram(client, p);
    }
    check(lastBlockCount == 2, "EP6 after the start decodes as two receivers");
    client.setBandscopeEnabled(false);

    // ================= 2: a second restart supersedes one in flight
    const double twoAt = nowMs();
    client.setReceiverCount(3);
    client.setReceiverCount(1);
    spin(80);
    {
        int stops = 0, starts = 0, lastCount = 0;
        for (const Ev& e : ev) {
            if (e.tMs < twoAt) continue;
            if (e.kind == 'S') ++stops;
            if (e.kind == 'R' && e.run == 0x01) ++starts;
            if (e.kind == 'C') lastCount = e.numRx;
        }
        check(stops == 2 && starts == 1,
              "back-to-back restarts: two stops, ONE start -- the first chain was cancelled");
        check(lastCount == 1, "and the wire carries the LAST receiver count asked for");
        check(MetisClientTestAccess::restartIdle(client), "the superseding restart completed");
    }

    // ================= 3: stop() inside the window cancels the rest
    client.setReceiverCount(2);
    const double stopAt = nowMs();
    client.stop();
    spin(60);
    {
        int runs = 0;
        for (const Ev& e : ev)
            if (e.tMs >= stopAt && e.kind == 'R')
                ++runs;
        check(runs == 0, "stop() inside the window: no start goes out afterwards");
        check(MetisClientTestAccess::restartIdle(client), "and nothing is left in flight");
    }

    std::fprintf(stderr, "hl2_receiver_count_restart_paced_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
