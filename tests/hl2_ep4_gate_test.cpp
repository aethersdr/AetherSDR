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

#include "Hl2Ep4ArrivalsD94.h"

#include <QCoreApplication>
#include <QSignalSpy>

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
    static void setSampleRate(MetisClient& client, SampleRate rate)
    {
        client.m_params.sampleRate = rate;
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

        check(kD94Ep4SeqToggleCount == 3048, "the recorded toggle leg holds 3048 arrivals");
        check(kD94Ep4SeqToggle[0] == 0 && kD94Ep4SeqToggle[3047] == 3047,
              "...numbered 0 to 3047");

        std::size_t at = 0;
        for (std::size_t k = 0; k < kD94Ep4ToggleCycleCount; ++k) {
            const D94Ep4ToggleCycle& cyc = kD94Ep4ToggleCycles[k];
            check(kD94Ep4SeqToggle[at] == cyc.firstSeqAfterEnable,
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
        check(at == kD94Ep4SeqToggleCount, "the whole recording was replayed");
        check(spy.count() == 4, "four cycles, four blocks");

        // The whole point of the alignment rule: nothing in the sequence
        // accounting would ever have complained.
        check(c.ep4Drops() == 0, "3048 recorded arrivals across four cycles lost nothing");
        check(c.ep4Rewinds() == 0, "...and never rewound: `run` never dropped");
        check(c.linkCounters().ep4Packets == kD94Ep4SeqToggleCount,
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

        c.setMox(true);
        check(MetisClientTestAccess::idle(c),
              "keying abandons the cycle in flight rather than finishing it");
        check(spy.count() == 0, "half a clean block merged with half a keyed one is not a reading");

        // And nothing re-arms while keyed.
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "no arming under MOX");
        check(c.bandscopeEnabled(), "...but the operator's intent is untouched");

        // Nor inside the post-unkey hold-off: d83 measured the transient at
        // 178-285 ms past the falling edge.
        c.setMox(false);
        MetisClientTestAccess::tick(c);
        check(MetisClientTestAccess::idle(c), "no arming inside the post-unkey hold-off");
        check(c.bandscopeTimeouts() == 0, "a refused arming is not a timeout");
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
        c.setMox(true);                        // refused: enableTransmit was never called
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

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
