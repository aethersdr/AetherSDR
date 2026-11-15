#pragma once

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Receive S-meter read from WDSP's RXA for host-DSP backends: tap constants,
// the block countdown that keeps the backend's own silence off the needle,
// rate-independent cadence, and publish-side ballistics. Shared by
// Hl2RxDsp/AnanRxDsp and Hl2Backend/AnanBackend because the numbers belong to
// WDSP's meter and the widget, not either radio.
// Threading: WdspSMeterTap is DSP-thread state; SMeterSmoother is
// publisher-thread state, per receiver. Neither is shared.
namespace AetherSDR {

struct WdspSMeter final {
    // WDSP's signal-average meter is an EMA over the channel's own samples
    // with this time constant, fixed where RXA builds it (upstream RXA.c,
    // create_meter's "averaging time constant" argument). Nothing flushes the
    // average when a stage starts or stops feeding zeros (upstream meter.c:
    // only flush_meter() touches `avg`, and it would set it to 0, i.e.
    // -400 dB), so the average carries silence ACROSS a mute's release edge,
    // and a fresh channel starts from that same zero.
    static constexpr double kAverageTauSec = 0.100;
    // Three time constants: the average is then within
    // 10*log10(1 - e^-3) = 0.22 dB of its settled value, well inside one
    // S-unit. Not stretched further -- the whole cost of the window is that
    // the last good reading is held for its length, and past about a third of
    // a second the needle visibly lags the band coming back.
    static constexpr int kSettleTaus = 3;

    // Blocks to swallow after the backend's own silence or a channel install.
    // Blocks, not milliseconds: the meter advances per processed block and
    // nothing else clocks it, so counting blocks measures the same clock the
    // average integrates on -- a wall clock would expire early on a stalled
    // stream and publish exactly the reading this exists to withhold. Sized
    // from the input rate so every rate waits the same wall-clock time. At
    // least one block whatever the arguments say.
    [[nodiscard]] static int settleBlocks(int inputSampleRateHz,
                                          int dspBlockSize) noexcept
    {
        if (inputSampleRateHz <= 0 || dspBlockSize <= 0)
            return 1;
        const double blocks = kSettleTaus * kAverageTauSec
            * static_cast<double>(inputSampleRateHz)
            / static_cast<double>(dspBlockSize);
        return std::max(1, static_cast<int>(std::ceil(blocks)));
    }

    // Blocks between readings. A block is a fixed count of input samples, so its
    // wall time shrinks with input rate (1024 samples: 21 ms at 48 ksps, 0.67 ms at
    // 1536 ksps). Reading every inputRate/dspRate-th block keeps ~47 readings/s at
    // any rate, so the publisher's EMA (τ ~140 ms) doesn't change with zoom.
    [[nodiscard]] static int emitEveryBlocks(int inputSampleRateHz,
                                             int dspSampleRateHz) noexcept
    {
        if (inputSampleRateHz <= 0 || dspSampleRateHz <= 0)
            return 1;
        return std::max(1, inputSampleRateHz / dspSampleRateHz);
    }
};

// The tap's gate, on the DSP thread. arm() on a channel install and on the
// mute's release edge; tick() once per block WDSP actually completed on the
// UNMUTED path, and read the meter only when it says so.
class WdspSMeterTap final {
public:
    // Re-arms, never accumulates: a rate change arms this twice (install,
    // then the unmute) and waits one window, not two.
    void arm(int inputSampleRateHz, int dspBlockSize, int dspSampleRateHz) noexcept
    {
        m_settleBlocks = WdspSMeter::settleBlocks(inputSampleRateHz, dspBlockSize);
        m_every = WdspSMeter::emitEveryBlocks(inputSampleRateHz, dspSampleRateHz);
        m_sinceRead = 0;
    }
    // True when this block's reading should be read and emitted.
    [[nodiscard]] bool tick() noexcept
    {
        if (m_settleBlocks > 0) {
            --m_settleBlocks;
            return false;
        }
        if (++m_sinceRead < m_every)
            return false;
        m_sinceRead = 0;
        return true;
    }
    [[nodiscard]] int settleBlocksRemaining() const noexcept { return m_settleBlocks; }

private:
    int m_settleBlocks = 0;
    int m_every = 1;
    int m_sinceRead = 0;
};

// Publish-side ballistics: smooth EVERY reading, publish only on the tick.
// Both halves matter. Smoothing all of them is what makes the published value
// represent the whole interval rather than one arbitrary instant inside it,
// and the tick is what stops ~47 cross-thread emits a second repainting a
// needle nobody can read that fast. Dropping readings without smoothing would
// alias -- the meter would show whichever instant landed on the tick.
class SMeterSmoother final {
public:
    // 100 ms is the cadence MetisClient publishes radio telemetry at
    // (kTelemetryMinIntervalMs), so every host-DSP meter updates on one clock.
    static constexpr std::int64_t kPublishIntervalMs = 100;
    // Flex's own meter ballistics, from MeterModel's forward-power smoothing:
    // fast attack so a peak is not missed, slow decay so the needle settles.
    // Reused rather than re-invented so an operator moving between radios
    // sees meters that behave the same way.
    static constexpr double kAttackAlpha = 0.5;
    static constexpr double kDecayAlpha  = 0.15;

    // A new session's needle starts from its first reading, not from where
    // the last session's left off, and that first reading publishes at once.
    void reset() noexcept
    {
        m_have = false;
        m_published = false;
    }

    // One reading in; the value to publish out, if the tick has come round.
    // The clock is the caller's so the arithmetic is checkable without
    // waiting on one -- see feed() for the production entry point.
    [[nodiscard]] std::optional<double> feedAt(double dbm, std::int64_t nowMs) noexcept
    {
        if (!m_have) {
            m_dbm = dbm;
            m_have = true;
        } else {
            const double alpha = (dbm > m_dbm) ? kAttackAlpha : kDecayAlpha;
            m_dbm = alpha * dbm + (1.0 - alpha) * m_dbm;
        }
        if (m_published && nowMs - m_lastPublishMs < kPublishIntervalMs)
            return std::nullopt;
        m_published = true;
        m_lastPublishMs = nowMs;
        return m_dbm;
    }

    [[nodiscard]] std::optional<double> feed(double dbm) noexcept
    {
        if (!m_clock.isValid())
            m_clock.start();
        return feedAt(dbm, m_clock.elapsed());
    }

    // The smoothed value itself, whether or not the tick has come round.
    [[nodiscard]] double value() const noexcept { return m_dbm; }

private:
    QElapsedTimer m_clock;
    double m_dbm = 0.0;
    bool m_have = false;
    bool m_published = false;
    std::int64_t m_lastPublishMs = 0;
};

}  // namespace AetherSDR
