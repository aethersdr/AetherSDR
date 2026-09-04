#pragma once

#include <algorithm>
#include <cstdint>

namespace AetherSDR {

// TX_CHRONO pull-pacer for TCI digital-mode audio (#5133).
//
// WSJT-X / JTDX send one TX_AUDIO block per type-3 chrono. Two failure
// modes have shown up on the air:
//
//   * Unbounded catch-up: a GUI stall is repaid as N chrono frames in one
//     tick → a clump of TX_AUDIO hits Flex dax_tx / Icom TxPacketizer
//     (1–2 s FT8 spur).
//   * Dropping owed time: capping the accumulator at two periods under a
//     loaded 5K-display GUI (~20–40 ms ticks) discarded ~25 % of the
//     clock (effective48k ≈ 36 k) → Icom underflow holes → 7–8 spurs.
//
// Thetis (TCI authority) caps outstanding requests and forgets stuck
// counts after max(250 ms, 4×buffering). OpusTxPacer already solved the
// sibling problem with kMaxPacketsPerDrain = 3. This pacer:
//
//   * pays back stalls at most three frames per poll (bounded catch-up)
//   * keeps up to ~213 ms of backlog so a 5K hitch is repaid, not dropped
//   * only discards pathological backlogs (seconds)
//   * refuses new chronos while too many are unanswered
//   * forgets a stuck outstanding count after 250 ms without TX_AUDIO
class TciTxChronoPacer {
public:
    static constexpr int kStereoFrames = 1024;
    static constexpr int kSampleRateHz = 48000;
    static constexpr std::int64_t kPeriodNs =
        (static_cast<std::int64_t>(kStereoFrames) * 1000000000LL)
        / static_cast<std::int64_t>(kSampleRateHz);
    // Same bound OpusTxPacer uses for a late audio-thread drain.
    static constexpr int kMaxFramesPerTick = 3;
    // Ten periods (~213 ms). A 50–80 ms 5K/GPU hitch fits; a 1.6 s
    // watchdog stall does not get replayed in full.
    static constexpr std::int64_t kMaxBacklogNs = 10 * kPeriodNs;
    // ~170 ms of unanswered chrono. Under Icom TxPacketizer's 250 ms cap
    // so a catch-up clump cannot overflow the radio queue.
    static constexpr int kMaxOutstanding = 8;
    static constexpr std::int64_t kForgetStuckNs = 250000000; // 250 ms

    struct TickResult {
        int framesToSend{0};
        int wouldHaveSent{0};   // unbounded while-loop count (telemetry)
        std::int64_t droppedNs{0};
        bool forgotStuck{false};
        bool outstandingCapped{false};
        std::int64_t accumNs{0};
        int outstanding{0};
    };

    void reset()
    {
        m_accumNs = 0;
        m_outstanding = 0;
    }

    // elapsedNs: nsecs since the previous poll (or since start).
    // audioGapNs: nsecs since the last TX_AUDIO block; <0 if none this over.
    TickResult tick(std::int64_t elapsedNs, std::int64_t audioGapNs)
    {
        TickResult out;
        if (elapsedNs < 0) {
            elapsedNs = 0;
        }
        m_accumNs += elapsedNs;

        if (m_accumNs >= kPeriodNs) {
            out.wouldHaveSent = static_cast<int>(m_accumNs / kPeriodNs);
        }

        if (m_outstanding > 0 && audioGapNs >= kForgetStuckNs) {
            m_outstanding = 0;
            out.forgotStuck = true;
        }

        const int owed = (m_accumNs >= kPeriodNs)
            ? static_cast<int>(m_accumNs / kPeriodNs)
            : 0;
        const int room = kMaxOutstanding - m_outstanding;
        int send = std::min({owed, kMaxFramesPerTick, std::max(room, 0)});
        if (owed > 0 && send == 0 && m_outstanding >= kMaxOutstanding) {
            out.outstandingCapped = true;
        }
        if (send > 0) {
            m_accumNs -= static_cast<std::int64_t>(send) * kPeriodNs;
        }
        out.framesToSend = send;

        // Drop only after paying this tick, and only the pathological tail.
        if (m_accumNs > kMaxBacklogNs) {
            out.droppedNs = m_accumNs - kMaxBacklogNs;
            m_accumNs = kMaxBacklogNs;
        }

        out.accumNs = m_accumNs;
        out.outstanding = m_outstanding;
        return out;
    }

    void onChronoSent()
    {
        if (m_outstanding < kMaxOutstanding) {
            ++m_outstanding;
        }
    }

    void onAudioBlock()
    {
        if (m_outstanding > 0) {
            --m_outstanding;
        }
    }

    std::int64_t accumNs() const { return m_accumNs; }
    int outstanding() const { return m_outstanding; }

private:
    std::int64_t m_accumNs{0};
    int m_outstanding{0};
};

} // namespace AetherSDR
