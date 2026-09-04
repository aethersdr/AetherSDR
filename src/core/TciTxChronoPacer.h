#pragma once

#include <cstdint>

namespace AetherSDR {

// TX_CHRONO pull-pacer for TCI digital-mode audio (#5133).
//
// WSJT-X / JTDX send one TX_AUDIO block per type-3 chrono. The old TciServer
// loop repaid a GUI stall as an unbounded burst of chrono frames on the next
// 5 ms tick, which dumped a clump of audio onto Flex dax_tx / Icom TxPacketizer
// and showed up as a 1–2 s FT8 spur. Thetis (TCI authority) caps outstanding
// requests and forgets stuck counts after max(250 ms, 4×buffering).
//
// This pacer:
//   * emits at most one chrono per poll tick
//   * caps the time accumulator so a stall is dropped, not replayed
//   * refuses new chronos while too many are unanswered
//   * forgets a stuck outstanding count after 250 ms without TX_AUDIO
class TciTxChronoPacer {
public:
    static constexpr int kStereoFrames = 1024;
    static constexpr int kSampleRateHz = 48000;
    static constexpr std::int64_t kPeriodNs =
        (static_cast<std::int64_t>(kStereoFrames) * 1000000000LL)
        / static_cast<std::int64_t>(kSampleRateHz);
    static constexpr int kMaxFramesPerTick = 1;
    // Two periods (~42.7 ms): enough to absorb one 5 ms poll jitter, not a
    // waterfall stall. Excess is dropped.
    static constexpr std::int64_t kMaxBacklogNs = 2 * kPeriodNs;
    // ~64 ms of unanswered chrono. Thetis allows 64; that is the burst we are
    // closing. Three is one extra after a missed tick plus margin.
    static constexpr int kMaxOutstanding = 3;
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

        if (m_accumNs > kMaxBacklogNs) {
            out.droppedNs = m_accumNs - kMaxBacklogNs;
            m_accumNs = kMaxBacklogNs;
        }

        if (m_outstanding > 0 && audioGapNs >= kForgetStuckNs) {
            m_outstanding = 0;
            out.forgotStuck = true;
        }

        if (m_accumNs >= kPeriodNs) {
            if (m_outstanding >= kMaxOutstanding) {
                out.outstandingCapped = true;
            } else {
                out.framesToSend = kMaxFramesPerTick;
                m_accumNs -= kPeriodNs;
            }
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
