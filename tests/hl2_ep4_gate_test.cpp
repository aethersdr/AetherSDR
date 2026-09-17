// HL2 wideband bandscope (endpoint 0x04) — the DUTY-CYCLE GATE. Socket-free:
// no bind, no peer, no radio, no event loop. Datagrams are handed straight to
// the drain path through MetisClientTestAccess, and the two timers are fired by
// hand so the state machine is exercised deterministically rather than by
// waiting on a clock.
//
// The claim under test is that the gate keeps ONE block per arming cycle and
// that the block is a contiguous 2048-sample record of the radio's present,
// which is harder than it sounds for three measured reasons:
//
//   * a mid-stream re-enable does NOT re-align ep4_seq_no, so the gate must
//     wait for a block boundary rather than accept the first packet it sees —
//     and the sequence numbers give it no warning, being continuous throughout;
//   * the arming delay is constant in EP6 PACKETS, not in seconds, so a guard
//     expressed in milliseconds alone fires always at one sample rate and never
//     at another;
//   * the disable yields exactly one trailing packet, which belongs to the
//     block already emitted and must not seed the next one.
//
// The enable/disable cycles replayed here are recorded arrivals from a real
// v74.2 board (tests/Hl2Ep4ArrivalsD94.h, the midstream-toggle leg of bench run
// d94-ep4-bandscope-existence), not invented ones.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "TxTestAuthority.h"

#include "Hl2Ep4ArrivalsD94.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {
// No start(), no bind, no peer: inject transport state, feed the ingest path
// the bytes a socket would have delivered, and fire the gate's two timers by
// hand. Firing them directly is what makes the test deterministic AND is the
// only way to reach onBandscopeGuardTimeout without waiting 420 ms of wall
// clock for every case that needs it.
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void feedDatagram(MetisClient& client, std::span<const std::uint8_t> bytes)
    {
        client.handleDatagram(bytes);
    }
    static void tick(MetisClient& client) { client.onBandscopeTick(); }
    static void fireGuard(MetisClient& client) { client.onBandscopeGuardTimeout(); }
    static bool idle(MetisClient& client)
    {
        return client.m_bsState == MetisClient::BandscopeState::Idle;
    }
    static bool arming(MetisClient& client)
    {
        return client.m_bsState == MetisClient::BandscopeState::Arming;
    }
    static bool trailingPending(MetisClient& client) { return client.m_bsTrailingPending; }
    // THE BYTE THE GATE COMPOSED, not the byte a socket sent: sendBandscopeRunByte
    // records it before its `!m_socket` return, which is what makes the call
    // site's ARGUMENTS assertable at this layer at all.
    static std::uint8_t lastRunByte(MetisClient& client)
    {
        return client.m_lastBandscopeRunByte;
    }
    static void setSampleRate(MetisClient& client, SampleRate rate)
    {
        client.m_params.sampleRate = rate;
    }
    // What a first EP6 datagram sets: the link is up and the silence clock is
    // running. Injected rather than fed so the watchdog path can be driven
    // without an EP6 frame builder in this file — these are the only two
    // members handleDatagram touches on that edge.
    static void setLinkUp(MetisClient& client)
    {
        client.m_linkUp = true;
        client.m_sinceLastEp6.restart();
    }
    static void watchdogTick(MetisClient& client) { client.onWatchdogTick(); }
    static int silenceTimeoutMs() noexcept { return MetisClient::kSilenceTimeoutMs; }
    // What a telemetry response carrying a ptt_resp TRANSITION does: the bit
    // the interlock reads, plus the edge handler handleDatagram would call.
    // Injected rather than framed so the radio-keyed path can be driven without
    // an EP6 C&C builder in this file.
    static void setRadioPtt(MetisClient& client, bool keyed)
    {
        if (client.m_telemetry.ptt == keyed)
            return;
        client.m_telemetry.ptt = keyed;
        client.onRadioPttEdge(keyed);
    }
    // The post-unkey hold-off, elapsed. bandscopeInterlocked() treats an
    // INVALID m_sinceUnkey and an expired one identically — both mean "no
    // transient to wait out" — so invalidating it is the hold-off expiring,
    // without spending 300 ms of wall clock to say so.
    static void expireUnkeyHoldoff(MetisClient& client) { client.m_sinceUnkey.invalidate(); }
    static int maxConsecutiveTimeouts() noexcept
    {
        return MetisClient::kMaxConsecutiveBandscopeTimeouts;
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// An EP4 datagram whose 512 samples all carry the SAME code, derived from the
// sequence number. That is what makes "which four packets went into the block"
// an observable: Ep4Stats::merge takes the MAX of peakAbs, so a block's peak is
// the code of its highest-numbered packet and names it uniquely.
//
// kEp4FullScale is 2048 and the modulus is a multiple of kEp4PacketsPerBlock,
// so a block never straddles the fold.
static int codeForSeq(std::uint32_t seq)
{
    return static_cast<int>(seq % static_cast<std::uint32_t>(kEp4FullScale));
}

static std::vector<std::uint8_t> makeEp4(std::uint32_t seq)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x04;
    pkt[4] = 0x00;                                            // hardwired
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0x0F);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    // The wire word is the 12-bit code shifted left by four, little-endian.
    const std::uint16_t word = static_cast<std::uint16_t>(codeForSeq(seq) << 4);
    for (std::size_t i = 0; i < kEp4SamplesPerPacket; ++i) {
        pkt[8 + 2 * i]     = static_cast<std::uint8_t>(word & 0xFF);
        pkt[8 + 2 * i + 1] = static_cast<std::uint8_t>((word >> 8) & 0xFF);
    }
    return pkt;
}

static void feed(MetisClient& c, std::uint32_t seq)
{
    MetisClientTestAccess::feedDatagram(c, makeEp4(seq));
}

// The peak of a block built from four consecutive packets starting at `first`.
static int expectedPeak(std::uint32_t first)
{
    return codeForSeq(first + 3);
}

// The four codes a delivered frame's 2048 samples carry, one per 512-sample
// run. A frame is only a record of ONE hardware block if those are four
// CONSECUTIVE codes, which is the property makeEp4 was built to make visible.
static std::vector<int> frameCodes(const QList<float>& samples)
{
    std::vector<int> codes;
    if (samples.size() != kEp4BlockSamples)
        return codes;
    for (int run = 0; run < kEp4PacketsPerBlock; ++run) {
        const float v = samples.at(run * static_cast<int>(kEp4SamplesPerPacket));
        codes.push_back(static_cast<int>(std::lround(v * kEp4FullScale)));
    }
    return codes;
}

static bool consecutive(const std::vector<int>& codes)
{
    if (codes.size() != static_cast<std::size_t>(kEp4PacketsPerBlock))
        return false;
    for (std::size_t i = 1; i < codes.size(); ++i) {
        if (codes[i] != codes[i - 1] + 1)
            return false;
    }
    return true;
}

// Feed packets from `from` until the gate has emitted one block, or `limit`
// packets have gone by. Returns the sequence number one past the last fed.
static std::uint32_t feedUntilBlock(MetisClient& c, QSignalSpy& spy,
                                    std::uint32_t from, int limit)
{
    const int before = spy.count();
    std::uint32_t seq = from;
    for (int i = 0; i < limit && spy.count() == before; ++i)
        feed(c, seq++);
    return seq;
}

int main(int argc, char** argv)
{
    // MetisClient::setMox carries its admitting operation; these cases are
    // about the transmit GATE, so a permanently-valid authority is the inert
    // constant that leaves the gate as the only variable.
    TxTestAuthority authority;
    QCoreApplication app(argc, argv);

    // ---- 1 · the guard is sized in the gateware's own units ----
    //
    // §5.5 of the study sized it at "10x the predicted block interval" — 105 ms
    // at 1 RX. That is right for a MID-STREAM enable (2.41-2.61 ms, four of
    // four) and WRONG for the other path into Arming: when Params::bandscope is
    // carried through setReceiverCount()'s stop/start, the run byte goes
    // 0x00 -> 0x03 and the first EP4 packet arrives 129 EP6 packets later.
    //
    // 129 PACKETS, NOT 129 MILLISECONDS. The SAME COUNT was measured at both
    // rates — 129 at 48 kHz and 129 at 384 kHz — while the wall-clock latencies
    // were 0.3297 s and 0.0418 s. A 105 ms guard therefore abandons every cycle
    // at 48 kHz and none at 384: a spurious failure whose presence depends on
    // the operator's sample rate.
    {
        // The delay expressed in the units it is actually constant in, and
        // computed from this header's own packet geometry so it cannot drift
        // from the arithmetic the guard uses.
        const double armingMs48  = 129 * ep6PacketIntervalMs(48000, 1);
        const double armingMs384 = 129 * ep6PacketIntervalMs(384000, 1);
        check(armingMs48 > 338.0 && armingMs48 < 339.0,
              "129 EP6 packets at 48 kHz / 1 RX is 0.339 s — the measured 0.3297 s and more");
        check(armingMs384 > 42.0 && armingMs384 < 42.5,
              "the same 129 packets at 384 kHz are 0.042 s — the measured 0.0418 s and more");

        // THE STUDY'S CONSTANT, computed here so the failure it would cause is
        // visible rather than described.
        const double studyGuardMs = 10.0 * kEp4BlockIntervalMs;
        check(studyGuardMs > 104.0 && studyGuardMs < 106.0,
              "the study's guard is 10 block intervals at 1 RX: 105 ms");
        check(studyGuardMs < armingMs48,
              "which does NOT cover the 48 kHz arming delay");
        check(studyGuardMs > armingMs384,
              "...and does cover the 384 kHz one, which is why it looked correct");

        check(bandscopeGuardMs(48000, 1) == 420,
              "the guard at 48 kHz / 1 RX is 160 packet intervals: 420 ms");
        check(static_cast<double>(bandscopeGuardMs(48000, 1)) > armingMs48,
              "...which clears the measured arming delay");
        check(bandscopeGuardMs(384000, 1) == 125,
              "the guard at 384 kHz / 1 RX falls back to the block-interval term");
        check(static_cast<double>(bandscopeGuardMs(384000, 1)) > armingMs384,
              "...which clears the measured arming delay there too");

        // THE BLOCK TERM IS SIZED ON THE SLOWEST RATE MEASURED, NOT ON THE
        // FASTEST. d94 found 380.95 EP4 packets/s at one receiver and called it
        // flat; d95 found 320.0 at three, exact to the datagram in three
        // separate legs. The bandscope loses START arbitration to EP6, so the
        // rate is flat in SAMPLE RATE and is not flat in RECEIVER COUNT — and a
        // deadline has to take the slowest cadence that has been observed.
        check(kEp4BlockIntervalSlowestMs > kEp4BlockIntervalMs,
              "three receivers make the bandscope's own block interval LONGER");
        check(kEp4BlockIntervalSlowestMs > 12.4 && kEp4BlockIntervalSlowestMs < 12.6,
              "320.0 packets/s at 3 RX is a 12.5 ms block interval");

        // The packet term falls with the receiver count, because EP6 packets
        // get smaller and more frequent, so 129 of them is less time. The block
        // term does not move at all — it is the slowest cadence observed at any
        // receiver count, which is what a deadline needs it to be.
        check(bandscopeGuardMs(48000, 3) < bandscopeGuardMs(48000, 1),
              "more receivers means faster EP6 packets and a shorter packet term");
        check(bandscopeGuardMs(48000, 3) >= 125,
              "...but never below the block-interval floor");
        check(bandscopeGuardMs(384000, 3) == 125,
              "at 384 kHz and 3 RX the floor is all there is");
    }

    // ---- 2 · the client's guard follows its own configuration ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        check(c.bandscopeGuardIntervalMs() == 420,
              "a client at its default 48 kHz / 1 RX guards for 420 ms");
        MetisClientTestAccess::setSampleRate(c, SampleRate::R384k);
        check(c.bandscopeGuardIntervalMs() == 125,
              "and at 384 kHz for 125 ms — the same 129 packets, a much shorter wait,"
              " so the block-interval floor takes over");
    }

    // ---- 3 · off by default, and a stopped client cannot be armed ----
    {
        MetisClient c;
        check(!c.bandscopeEnabled(), "the gate is off in a fresh client");
        c.setBandscopeEnabled(true);
        check(!c.bandscopeEnabled(), "enabling a stopped client is refused, not latched");
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "and a tick on a stopped client arms nothing");
    }

    // ---- 4 · ONE BLOCK PER ARMING, and it is the SECOND aligned block ----
    //
    // The first block after wide_spectrum goes up is whatever was already
    // sitting in the 2048-word capture FIFO: its samples predate the enable by
    // an unknown amount. It is flushed. The block kept is the next one.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        check(c.bandscopeEnabled(), "enabling a running client starts the gate");
        check(!MetisClientTestAccess::idle(c), "and arms immediately, not a second from now");
        // THE ARGUMENTS, not the bits: metisRunCommand's encoding is pinned in
        // hl2_metis_protocol_test. What is unpinned without this is which
        // arguments the gate hands it — and `run` dropped here stops the IQ the
        // operator is listening to. Masked with 0x03 so the check does not
        // depend on the watchdog-disable bit.
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x03,
              "arming sends run|wide_spectrum");

        for (std::uint32_t s = 0; s < 8; ++s) {
            feed(c, s);
            if (s < 7)
                check(spy.count() == 0, "no block is emitted before its fourth packet");
        }
        check(spy.count() == 1, "exactly one block, on the fourth packet of the second");
        const auto block = spy.at(0).at(0).value<Ep4Stats>();
        check(block.samples == kEp4BlockSamples, "a block is 2048 samples, not 512");
        check(block.peakAbs == expectedPeak(4),
              "the block kept is packets 4..7 — the stale one, 0..3, was flushed");
        check(c.bandscopeBlocks() == 1, "and the counter agrees");

        // And the gate is down again: everything after this belongs to no block
        // until the next sampling period.
        check(MetisClientTestAccess::idle(c), "the gate lowers the bit as soon as it has one");
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x01,
              "the lowering byte keeps run set");
        for (std::uint32_t s = 8; s < 40; ++s)
            feed(c, s);
        check(spy.count() == 1, "a radio that keeps sending does not make the gate keep blocks");
    }

    // ---- 5 · THE RECORDED TOGGLE, replayed ----
    //
    // Four real enable/disable cycles from a v74.2 board, including the two
    // that resume at seq % 4 == 2. Each cycle: arm the gate, feed the recorded
    // arrivals of that cycle, and check which four packets became the reading.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);

        check(kD94Ep4ToggleArrivals == 3048, "the recorded toggle leg holds 3048 arrivals");
        check(kD94Ep4ToggleFirstSeq == 0 && kD94Ep4ToggleLastSeq == 3047,
              "...numbered 0 to 3047, with no gap");

        std::size_t at = 0;
        for (std::size_t k = 0; k < kD94Ep4ToggleCycleCount; ++k) {
            const D94Ep4ToggleCycle& cyc = kD94Ep4ToggleCycles[k];
            check(at == static_cast<std::size_t>(cyc.firstSeqAfterEnable),
                  "the recording resumes where the cycle table says it does");
            check(static_cast<int>(cyc.firstSeqAfterEnable % 4) == cyc.firstSeqPhase,
                  "...on the recorded phase");

            // The gate arms. In the recording the bench tool did this too; here
            // it is the sampling period's tick.
            if (k > 0) {
                check(MetisClientTestAccess::idle(c), "the previous cycle finished and lowered");
                MetisClientTestAccess::tick(c);
            }
            check(!MetisClientTestAccess::idle(c), "armed");

            // THE ALIGNMENT THE GATE MUST WAIT FOR. First block boundary at or
            // after the resume; the block kept is the one after that.
            const std::uint32_t firstAligned =
                (cyc.firstSeqAfterEnable + 3u) & ~3u;
            const std::uint32_t kept = firstAligned + 4u;

            const int before = spy.count();
            for (std::uint32_t s = cyc.firstSeqAfterEnable;
                 s <= cyc.lastSeqBeforeDisable; ++s)
                feed(c, s);
            check(spy.count() == before + 1,
                  "one cycle of the recording yields exactly one block");
            const auto block = spy.at(spy.count() - 1).at(0).value<Ep4Stats>();
            check(block.samples == kEp4BlockSamples, "2048 samples");
            check(block.peakAbs == expectedPeak(kept),
                  "the block is four IN-PHASE packets after the flush, not the first four seen");

            // THE TRAILING PACKET. One, always — the packet already inside
            // usopenhpsdr1.v's WIDE states when the disable landed.
            check(cyc.trailingPackets == 1, "the recording says one trailing packet");
            for (int t = 0; t < cyc.trailingPackets; ++t)
                feed(c, cyc.lastSeqBeforeDisable + 1u + static_cast<std::uint32_t>(t));
            check(spy.count() == before + 1, "the trailing packet is not a block");

            at = static_cast<std::size_t>(cyc.lastSeqBeforeDisable) + 2u;
        }
        check(at == kD94Ep4ToggleArrivals, "the whole recording was replayed");
        check(spy.count() == 4, "four cycles, four blocks");

        // The whole point of the alignment rule: nothing in the sequence
        // accounting would ever have complained.
        check(c.ep4Drops() == 0, "3048 recorded arrivals across four cycles lost nothing");
        check(c.ep4Rewinds() == 0, "...and never rewound: `run` never dropped");
        check(c.linkCounters().ep4Packets == kD94Ep4ToggleArrivals,
              "every recorded arrival was counted on the wire");
        check(c.bandscopeBlocks() == 4, "and four of them became readings");
    }

    // ---- 6 · the resume phase is not a curiosity: it is what the gate makes ----
    //
    // The gate's own duty cycle leaves the counter at phase 1 every time —
    // capture ends on phase 3, the trailing packet is phase 0, and the next
    // enable resumes at phase 1. So EVERY cycle but the first begins misaligned,
    // and the study's `seq % 4 == 0` exit is exercised on every sample the
    // sensor ever takes, not on an occasional unlucky one.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);

        std::uint32_t seq = feedUntilBlock(c, spy, 0, 16);
        check(spy.count() == 1, "the first cycle yields a block");
        check(seq == 8, "...on the eighth packet: four flushed, four kept");
        feed(c, seq++);                       // the trailing packet, seq 8, phase 0
        check(seq % 4 == 1, "the counter resumes at phase 1 after a gate cycle");

        MetisClientTestAccess::tick(c);
        seq = feedUntilBlock(c, spy, seq, 20);
        check(spy.count() == 2, "the second cycle yields a block too");
        const auto block = spy.at(1).at(0).value<Ep4Stats>();
        // Resume at 9 (phase 1) -> first boundary 12 -> flush 12..15 -> keep 16..19.
        check(block.peakAbs == expectedPeak(16),
              "and it is aligned to the gateware's block boundary, not to the enable");
    }

    // ---- 7 · THE TRAILING PACKET MUST NOT SEED THE NEXT BLOCK ----
    //
    // In the ordinary case it lands 24-61 us after the disable, a second before
    // the next arming, and the Idle state ignores it. The case this guards is
    // the one d95 measured on the host side: a stalled reader, where the socket
    // queue is drained AFTER the timer that armed the next cycle.
    //
    // Then the stale packet reaches an ARMING gate carrying phase 0 — a perfect
    // block boundary, from a capture that ended before the enable. Accepted, it
    // would consume a flush slot the fresh stale block needed, and the emitted
    // reading would be of samples taken before the operator asked for them.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);

        const std::uint32_t next = feedUntilBlock(c, spy, 0, 16);
        check(spy.count() == 1 && next == 8, "first block: flushed 0..3, kept 4..7");
        check(MetisClientTestAccess::trailingPending(c),
              "the gate knows one more packet is still coming");

        // THE STALL: the next period's tick is serviced before the socket is
        // drained, so the gate arms and only then sees the trailing packet.
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::arming(c), "the next cycle armed while it was in flight");
        feed(c, 8);                          // the trailing packet — phase 0
        check(MetisClientTestAccess::arming(c),
              "a packet from the PREVIOUS capture does not open a block boundary");
        check(!MetisClientTestAccess::trailingPending(c), "and it is consumed exactly once");

        // The radio resumes at 9 after the re-enable: phase 1, boundary at 12,
        // flush 12..15, keep 16..19.
        for (std::uint32_t s = 9; s <= 19; ++s)
            feed(c, s);
        check(spy.count() == 2, "the second cycle completes");
        const auto block = spy.at(1).at(0).value<Ep4Stats>();
        check(block.peakAbs == expectedPeak(16),
              "and its samples are from after the enable, not from before it");
    }

    // ---- 8 · the guard abandons a cycle the radio never answered ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        check(!MetisClientTestAccess::idle(c), "armed");
        check(c.bandscopeTimeouts() == 0, "nothing timed out yet");

        MetisClientTestAccess::fireGuard(c);
        check(c.bandscopeTimeouts() == 1, "an unanswered arming is counted");
        check(c.linkCounters().bandscopeTimeouts == 1, "and published");
        check(MetisClientTestAccess::idle(c), "and the cycle is abandoned, not left open");
        check(spy.count() == 0, "no block is emitted from a cycle that never completed");
        // NO TRAILING PACKET IS EXPECTED. Nothing was ever inside the WIDE
        // states, so a flag set here would swallow the first packet of the next
        // cycle instead of the last of this one.
        check(!MetisClientTestAccess::trailingPending(c),
              "a cycle that saw no packet has no trailing packet to discard");

        // Proof that it does not: the next cycle's first packet is not eaten.
        MetisClientTestAccess::tick(c);
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);
        check(spy.count() == 1, "the cycle after a timeout completes normally");
        check(spy.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(4),
              "...on the right four packets");
    }

    // ---- 9 · a cycle abandoned mid-capture DOES expect its trailing packet ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 6; ++s)   // flushed 0..3, capturing 4, 5
            feed(c, s);
        MetisClientTestAccess::fireGuard(c);
        check(c.bandscopeTimeouts() == 1, "the incomplete capture is a timeout");
        check(MetisClientTestAccess::trailingPending(c),
              "packets WERE flowing, so the disable will yield one more");
    }

    // ---- 10 · the transmit interlocks ----
    //
    // The HL2 receives while it transmits and hears its own PA at enormous
    // strength. A block taken under MOX is a picture of us.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.enableTransmit(true);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 5; ++s)  // flushed 0..3, capturing
            feed(c, s);
        check(!MetisClientTestAccess::idle(c), "mid-capture");

        c.setMox(true, authority.operation);
        check(MetisClientTestAccess::idle(c),
              "keying abandons the cycle in flight rather than finishing it");
        check(spy.count() == 0, "half a clean block merged with half a keyed one is not a reading");
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x01,
              "keying lowers wide_spectrum, run still set");

        // And nothing re-arms while keyed.
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "no arming under MOX");
        check(c.bandscopeEnabled(), "...but the operator's intent is untouched");

        // Nor inside the post-unkey hold-off: d83 measured the transient at
        // 178-285 ms past the falling edge.
        c.setMox(false, authority.operation);
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "no arming inside the post-unkey hold-off");
        check(c.bandscopeTimeouts() == 0, "a refused arming is not a timeout");
    }

    // ---- 10b · keying while ARMING leaves no trailing packet to expect ----
    //
    // Section 10 keys MID-CAPTURE, where a trailing packet really is in flight.
    // The other case is the one the round-one fix was for, and nothing pinned
    // it: reverting setMox to bandscopeDisarm(/*expectTrailing=*/true) passed
    // all four targets. A cycle still ARMING has seen no EP4 packet, so there is
    // nothing inside usopenhpsdr1.v's WIDE states to flush, and a flag set here
    // would swallow the FIRST packet of the next cycle instead of the last of
    // this one. (PR #5650 review, K5PTB.)
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.enableTransmit(true);
        c.setBandscopeEnabled(true);
        check(MetisClientTestAccess::arming(c), "armed, no EP4 seen yet");
        c.setMox(true, authority.operation);
        check(!MetisClientTestAccess::trailingPending(c),
              "keying while ARMING leaves no flag to swallow the next cycle's first packet");
    }

    // ---- 11 · a refused key is not a key ----
    //
    // setMox(true) with the transmit gate closed leaves m_mox false, and the
    // bandscope must read that as "not transmitting" rather than as an edge.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        c.setMox(true, authority.operation);                        // refused: enableTransmit was never called
        check(!c.isKeyed(), "the transmit gate refused the key");
        check(!MetisClientTestAccess::idle(c), "so the cycle in flight is not abandoned");
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);
        check(spy.count() == 1, "and it completes");
    }

    // ---- 12 · a mid-block loss does not splice two hardware blocks together ----
    //
    // Four CONSECUTIVE in-phase packets or none: a gap inside a capture would
    // make the 2048 samples span two blocks with a hole between them, and
    // Ep4Stats has no way to say so.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (const std::uint32_t s : {0u, 1u, 2u, 3u, 4u, 5u, /* 6 lost */ 7u})
            feed(c, s);
        check(c.ep4Drops() == 1, "the loss is counted on the wire");
        check(spy.count() == 0, "and no block is emitted from the broken capture");
        check(MetisClientTestAccess::arming(c), "the gate waits for the next boundary");
        for (std::uint32_t s = 8; s < 16; ++s)
            feed(c, s);
        check(spy.count() == 1, "which arrives one flush and one capture later");
        check(spy.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(12),
              "flushed 8..11, kept 12..15");
    }

    // ---- 13 · disabling the gate stops it, and leaves the counters alone ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);
        check(spy.count() == 1 && c.bandscopeBlocks() == 1, "one block taken");

        c.setBandscopeEnabled(false);
        check(!c.bandscopeEnabled(), "the gate is stopped");
        check(MetisClientTestAccess::idle(c), "and idle");
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "a tick on a stopped gate arms nothing");
        for (std::uint32_t s = 9; s < 40; ++s)
            feed(c, s);
        check(spy.count() == 1, "and nothing that still arrives becomes a block");
        check(c.bandscopeBlocks() == 1, "the cumulative counter is not reset by stopping");
        check(c.linkCounters().ep4Packets == 39, "the wire counters keep counting");
    }

    // ---- 14 · a SILENCE-WATCHDOG link loss ends the gate's intent ----
    //
    // The path that is not stop(). onWatchdogTick() surfaces 2 s of EP6 silence
    // as link loss without tearing the session down, so m_running and
    // m_params.bandscope would both survive it — and if EP6 resumes before
    // RadioModel's 5 s reconnect timer fires, handleDatagram emits linkUp again
    // with no start() behind it. Hl2Backend's linkUp handler resets its mirrors
    // on the premise that a session begins with wide_spectrum clear, so the
    // health row would read OFF beside a gate still cycling and still publishing
    // ADC levels. The gate would also keep arming at a radio that has gone
    // quiet, manufacturing block timeouts. (PR #5650 review, K5PTB, who
    // reproduced the OFF-beside-a-fresh-reading row through a real Hl2Backend.)
    //
    // Real wall clock, and the ONE wait in this otherwise timer-injected file.
    // m_sinceLastEp6 is a QElapsedTimer: it can be started, restarted or
    // invalidated, but not backdated, and an invalid one makes onWatchdogTick
    // return without ever reaching the silence branch — so there is no seam
    // that removes this. Making kSilenceTimeoutMs injectable would, and was
    // rejected: that constant is load-bearing for the start-retry budget
    // (4 * kStartRetryMs must stay inside it, see the retry lambda), and a
    // test-only setter on it invites exactly the drift that comment guards
    // against. Overshooting is safe — elapsed() only grows — so the wait is
    // robust under load rather than flaky. (Weighed again in review round 3.)
    {
        MetisClient c;
        QSignalSpy down(&c, &MetisClient::linkDown);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        MetisClientTestAccess::setStreaming(c);
        MetisClientTestAccess::setLinkUp(c);
        c.setBandscopeEnabled(true);
        c.requestBandscopeFrame();   // and something is waiting on a picture
        check(c.bandscopeEnabled(), "the gate is running on a live link");
        check(!MetisClientTestAccess::idle(c), "and armed");

        QThread::msleep(static_cast<unsigned long>(
            MetisClientTestAccess::silenceTimeoutMs() + 100));
        MetisClientTestAccess::watchdogTick(c);
        check(down.count() == 1, "the silence watchdog reported link loss");
        check(!c.bandscopeEnabled(),
              "...and the gate's standing intent ended with the link, as stop() ends it");
        check(MetisClientTestAccess::idle(c), "...leaving no cycle in flight");
        // ...AND TOLD THE RADIO SO. stop() lowers wide_spectrum for free —
        // metisStop()'s 0x00 clears both bits — but this path sends no
        // metis-stop, so without an explicit disarm the bit stays raised on a
        // radio that may still be alive, and the stream runs ungated for the
        // rest of the session while this row reads off. The state going Idle
        // is not the same claim. (PR #5650 review round 3.)
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x01,
              "...and lowered wide_spectrum ON THE WIRE, as stop() does");

        // The premise Hl2Backend's linkUp reset depends on: whatever brings the
        // link back, the gate is not still running behind it.
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "and nothing re-arms on the dead link");

        // The on-demand request is answered by this path too. resetBandscopeGate()
        // is noexcept and clears only the per-cycle latch, so without an explicit
        // failure here the caller waits for a frame that can never arrive.
        check(fails.count() == 1, "the outstanding frame request was ANSWERED, not dropped");
        check(!fails.at(0).at(0).toString().isEmpty(), "with a reason, not an empty string");
    }

    // ---- 15 · THE ON-DEMAND FRAME: one request, one record, nothing left running ----
    //
    // This is the display's whole cost model. The gate above is a standing 1 Hz
    // sampler for the headroom rows; a frame request is not that. It raises
    // wide_spectrum for the length of exactly one arming cycle and puts the
    // gate back where it found it, so a window left open costs nothing after
    // its frame has been drawn.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        QSignalSpy blocks(&c, &MetisClient::bandscopeBlockReady);

        check(!c.bandscopeEnabled(), "the standing gate is OFF, which is the point");
        c.requestBandscopeFrame();
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);

        check(frames.count() == 1, "one request yields exactly one frame");
        check(fails.count() == 0, "and no failure alongside it");
        const QList<float> samples = frames.at(0).at(0).value<QList<float>>();
        check(samples.size() == kEp4BlockSamples,
              "a frame is a whole 2048-sample block, never a packet");
        const std::vector<int> codes = frameCodes(samples);
        check(consecutive(codes),
              "and its four packets are consecutive: one hardware block, not a splice");
        check(!codes.empty() && codes.front() == 4,
              "the FLUSHED block (0..3) is not what was delivered — 4..7 is");
        check(blocks.count() == 1,
              "the statistics for that same block go out as usual");
        check(blocks.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(4),
              "and they describe the block the frame carries, not another one");

        check(MetisClientTestAccess::idle(c), "the gate is idle again");
        check(!c.bandscopeEnabled(), "and the standing gate was never turned on");
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c),
              "a period tick arms nothing: one request is one frame, not a subscription");
    }

    // ---- 16 · a second request while one is outstanding is not a second frame ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        c.requestBandscopeFrame();
        c.requestBandscopeFrame();
        c.requestBandscopeFrame();
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);
        check(frames.count() == 1, "three requests, one arming cycle, one frame");
        check(fails.count() == 0, "the extra requests are absorbed, not refused");
    }

    // ---- 17 · a request that cannot be served is ANSWERED, never dropped ----
    //
    // The gated sampler skips a cycle silently and resumes on the next tick. A
    // request has no next tick: something is waiting on a reply, and silence
    // there is a window that says "Waiting for a frame" forever.
    {
        MetisClient c;
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        c.requestBandscopeFrame();
        check(fails.count() == 1, "a request at a client that is not streaming fails at once");
        check(!fails.at(0).at(0).toString().isEmpty(), "with a reason, not an empty string");
    }
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        c.requestBandscopeFrame();
        feed(c, 0);   // the cycle starts, and then nothing more arrives
        MetisClientTestAccess::fireGuard(c);
        check(frames.count() == 0, "a cycle that never completed delivers no frame");
        check(fails.count() == 1, "the guard answers the request instead of leaving it open");
        check(c.bandscopeTimeouts() == 1, "and the timeout is counted where it always was");
        check(MetisClientTestAccess::idle(c), "the gate is not left armed");
    }

    // ---- 18 · a request landing MID-CAPTURE is served by one further cycle ----
    //
    // The samples of a block already being captured were never decoded — the
    // decision is latched when Capturing is entered — so serving the request
    // from what is left would deliver a record short by a packet. The retry is
    // bounded at exactly one, because the latch is taken from a flag that is by
    // then already set.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        QSignalSpy blocks(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);          // the standing sampler is running
        for (std::uint32_t s = 0; s < 5; ++s) // 0..3 flushed, 4 is capturing
            feed(c, s);
        check(blocks.count() == 0, "the first block is still being captured");
        c.requestBandscopeFrame();            // too late for THIS block

        for (std::uint32_t s = 5; s < 8; ++s)
            feed(c, s);
        check(blocks.count() == 1, "that block completes as statistics");
        check(frames.count() == 0, "but it carries no samples, so it is not the frame");

        for (std::uint32_t s = 8; s < 20; ++s)
            feed(c, s);
        check(frames.count() == 1, "the next cycle serves the request");
        check(fails.count() == 0, "without a spurious failure in between");
        check(consecutive(frameCodes(frames.at(0).at(0).value<QList<float>>())),
              "and delivers one contiguous block, not the remains of two");
    }

    // ---- 19 · the standing sampler alone decodes NO samples ----
    //
    // The headroom rows need statistics and not a picture, and decoding 2048
    // codes per block is work on the I/O thread. Nothing pays for it unless
    // something asked.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy blocks(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 8; ++s)
            feed(c, s);
        check(blocks.count() == 1, "the sampler takes its block");
        check(frames.count() == 0, "and emits no frame, because nothing requested one");
    }

    // ---- 15 · A HOLE OF A MULTIPLE OF FOUR IS STILL A HOLE ----
    //
    // `phase` is seq % kEp4PacketsPerBlock, so a loss of exactly four — or
    // eight, or twelve — leaves it intact and the phase test alone accepts a
    // packet from a LATER capture as the next one of this block. The gate then
    // emitted 2048 samples built from packets 4, 5, 10 and 11, two hardware
    // captures ~10 ms apart, and published them as one contiguous record while
    // ep4Drops had already counted the four that went missing. The drop count
    // ep4SeqStep() computes is now passed down and closes it.
    // (PR #5650 review round 3.)
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 4; ++s) feed(c, s);   // flush 0..3
        feed(c, 4);                                        // capture, phase 0
        feed(c, 5);                                        // capture, phase 1
        // 6, 7, 8, 9 are LOST on the wire. 10 is phase 2 — it aliases.
        feed(c, 10);
        feed(c, 11);
        check(c.ep4Drops() == 4, "the four lost packets are counted as drops");
        check(spy.count() == 0,
              "a block spanning a four-packet hole is NOT emitted as contiguous");
        // And the gate recovers on the next clean block rather than wedging.
        for (std::uint32_t s = 12; s < 20; ++s) feed(c, s);
        check(spy.count() == 1, "...and the next aligned block is taken normally");
        check(spy.count() == 1
                  && spy.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(16),
              "...from four genuinely consecutive packets");
    }

    // ---- 15b · the same hole, during the FLUSH ----
    //
    // Flushing has the identical phase test and therefore the identical blind
    // spot. It matters less — the flushed block is thrown away — but a flush
    // that silently resumed on a later capture would put the CAPTURE that
    // follows it on the wrong boundary, which is the thing section 15 is about.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        feed(c, 0);                                   // flush begins, phase 0
        // 1, 2, 3, 4 are LOST. 5 is phase 1 — it aliases onto the flush.
        for (std::uint32_t s = 5; s < 16; ++s) feed(c, s);
        check(c.ep4Drops() == 4, "the four lost packets are counted as drops");
        // With the hole rejected the flush restarts at 8 and the block kept is
        // 12..15. Accepting the alias would have flushed 0,5,6,7 and kept 8..11.
        check(spy.count() == 1, "one block");
        check(spy.count() == 1
                  && spy.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(12),
              "the flush restarted at the next boundary rather than resuming "
              "across the hole");
    }

    // ---- 16 · THE RADIO'S OWN PTT INTERLOCKS THE GATE ----
    //
    // m_mox is the final authority for keying THIS CLIENT initiated. It is not
    // the final authority for whether the PA is on the air: ptt_resp is
    // `cw_on | ext_ptt`, so a PTT jack, a foot switch or the radio's internal
    // keyer transmits without m_mox ever becoming true. A block taken then is
    // still a picture of us. (PR #5650 review round 3.)
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setBandscopeEnabled(true);
        check(!MetisClientTestAccess::idle(c), "armed on a quiet radio");

        MetisClientTestAccess::setRadioPtt(c, true);
        check(MetisClientTestAccess::idle(c),
              "the radio keying itself abandons the cycle, as MOX does");
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c),
              "...and nothing re-arms while the radio is keyed");
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x01,
              "...with wide_spectrum lowered and run still set");

        // Unkey: the post-unkey hold-off runs from the RADIO's falling edge.
        MetisClientTestAccess::setRadioPtt(c, false);
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c),
              "the measured post-unkey transient is still refused");
        // Past the hold-off, it resumes on its own — the operator's intent was
        // never touched.
        MetisClientTestAccess::expireUnkeyHoldoff(c);
        MetisClientTestAccess::tick(c);
        check(!MetisClientTestAccess::idle(c), "...and then the sensor resumes");
        check(c.bandscopeEnabled(), "the standing intent survived the whole thing");
    }

    // ---- 17 · A GATEWARE THAT NEVER ANSWERS EP4 IS GIVEN UP ON ----
    //
    // Without this the gate re-arms at 1 Hz forever on a board that does not
    // implement endpoint 0x04: two run-byte datagrams and one log line every
    // second for the life of the session, with nothing concluding anything.
    // The condition is narrow on purpose — a long run of timeouts AND not one
    // EP4 datagram in the whole session — so a lossy link never trips it.
    // (PR #5650 review round 3.)
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setBandscopeEnabled(true);
        for (int i = 0; i < MetisClientTestAccess::maxConsecutiveTimeouts(); ++i) {
            check(c.bandscopeEnabled(), "still trying");
            MetisClientTestAccess::fireGuard(c);
            MetisClientTestAccess::tick(c);
        }
        check(!c.bandscopeEnabled(),
              "a radio that answered nothing at all stops being asked");
        check(MetisClientTestAccess::idle(c), "...with no cycle left in flight");
        check((MetisClientTestAccess::lastRunByte(c) & 0x03) == 0x01,
              "...and wide_spectrum lowered on the way out");
        check(c.bandscopeTimeouts()
                  == static_cast<quint64>(MetisClientTestAccess::maxConsecutiveTimeouts()),
              "every abandoned cycle was counted");
    }

    // ---- 18 · ONE EP4 PACKET IS ENOUGH TO KEEP TRYING ----
    //
    // The other half of section 17, and the reason the condition is an AND: a
    // link that drops blocks but does deliver packets is a link problem, not a
    // gateware that lacks the endpoint, and the operator asked for this sensor.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setBandscopeEnabled(true);
        feed(c, 0);   // one arrival, ever
        for (int i = 0; i < MetisClientTestAccess::maxConsecutiveTimeouts() * 2; ++i) {
            MetisClientTestAccess::fireGuard(c);
            MetisClientTestAccess::tick(c);
        }
        check(c.bandscopeEnabled(),
              "a radio that HAS produced EP4 keeps being retried, however many "
              "cycles time out");
    }

    // ---- 19 · A TICK DOES NOT STACK A SECOND CYCLE ON A LIVE ONE ----
    //
    // The guard is shorter than the sampling period, so this is defensive — but
    // both are QTimers and either can slip under load, and re-arming mid-cycle
    // would re-send the run byte and restart the guard around a capture already
    // half-built. Nothing pinned it. (PR #5650 review round 3.)
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy spy(&c, &MetisClient::bandscopeBlockReady);
        c.setBandscopeEnabled(true);
        for (std::uint32_t s = 0; s < 6; ++s) feed(c, s);   // mid-capture
        check(!MetisClientTestAccess::idle(c), "a cycle is in flight");
        MetisClientTestAccess::tick(c);                     // the period comes round
        for (std::uint32_t s = 6; s < 8; ++s) feed(c, s);
        check(spy.count() == 1, "the cycle in flight completed rather than restarting");
        check(spy.count() == 1
                  && spy.at(0).at(0).value<Ep4Stats>().peakAbs == expectedPeak(4),
              "...on its own four packets, not a set the re-arm would have begun");
    }

    // Abandoning a one-shot capture must answer it and admit the next request.
    // Exercise all three gate phases and both PTT sources without a socket.
    for (const int packetsBeforeInterrupt : {0, 1, 5}) {
        for (const int edge : {0, 1, 2}) {
            MetisClient c;
            MetisClientTestAccess::setStreaming(c);
            QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
            QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
            if (edge == 1) {
                c.enableTransmit(true);
            } else if (edge == 2) {
                c.setBandscopeEnabled(true);
            }
            c.requestBandscopeFrame();
            for (int s = 0; s < packetsBeforeInterrupt; ++s) {
                feed(c, static_cast<std::uint32_t>(s));
            }
            if (edge == 0) {
                MetisClientTestAccess::setRadioPtt(c, true);
                MetisClientTestAccess::setRadioPtt(c, false);
            } else if (edge == 1) {
                c.setMox(true, authority.operation);
                c.setMox(false, authority.operation);
            } else {
                c.setBandscopeEnabled(false);
                c.setBandscopeEnabled(false); // repeated cancellation is idempotent
            }
            MetisClientTestAccess::expireUnkeyHoldoff(c);
            MetisClientTestAccess::fireGuard(c);
            check(fails.count() == 1, "an abandoned frame is answered exactly once");
            if (!fails.isEmpty()) {
                check(!fails.first().first().toString().isEmpty(),
                      "cancellation carries an operator-visible reason");
            }
            check(frames.count() == 0, "an interrupted capture delivers no frame");
            check(MetisClientTestAccess::idle(c), "cancellation disarms the gate");
            check(!c.bandscopeEnabled(), "recovery does not require a standing sampler");

            c.requestBandscopeFrame();
            check(MetisClientTestAccess::arming(c), "the next request can arm normally");
            // Include any trailing packet plus a flush and a whole fresh block.
            for (std::uint32_t s = 8; s < 24; ++s) {
                feed(c, s);
            }
            check(frames.count() == 1, "the next request delivers one complete frame");
            check(fails.count() == 1, "successful recovery does not fail another request");
        }
    }

    // A refused host PTT must not cancel a valid frame request.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        QSignalSpy frames(&c, &MetisClient::bandscopeFrameReady);
        QSignalSpy fails(&c, &MetisClient::bandscopeFrameFailed);
        c.requestBandscopeFrame();
        c.setMox(true, authority.operation); // transmit was never enabled; no wire or peer exists
        for (std::uint32_t s = 0; s < 8; ++s) {
            feed(c, s);
        }
        check(frames.count() == 1 && fails.count() == 0,
              "a refused key leaves the frame request intact");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
