#pragma once

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// The receive S-meter as read from WDSP's RXA and published by a host-DSP
// backend: the tap's constants, the block countdown that keeps the backend's
// own silence off the needle, the cadence that keeps the reading rate the
// same at every input rate, and the publish-side ballistics.
//
// WHY ONE HEADER. Hl2RxDsp and AnanRxDsp are two copies of the same receive
// stage, and Hl2Backend and AnanBackend two copies of the same publisher.
// Both pairs carried this arithmetic separately, and the settle copies had
// already diverged on the degenerate case (0 blocks against 1) before either
// had shipped. WdspProcessTally.h exists for the same reason one stage lower.
// The numbers here are properties of WDSP's meter and of the meter widget,
// not of either radio, so they live once.
//
// THREADING. WdspSMeterTap is DSP-thread state, touched only from the block
// loop and the setters that run there. SMeterSmoother is publisher-thread
// state, per receiver where a backend has more than one. Neither is shared.
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

    // Blocks between readings. Both stages feed WDSP a fixed number of INPUT
    // samples per block and hold dsp_rate constant, so a block is a shorter
    // slice of wall time the wider the receiver runs: 1024 samples is 21 ms
    // at 48 ksps and 0.67 ms at 1536 ksps. Read on every block, the publisher
    // would see ~47 readings a second at the narrowest rate and ~1500 at the
    // widest, and its per-reading EMA would lose its smoothing as the
    // operator zooms out (decay time constant ~140 ms -> ~4.5 ms). Reading
    // every inputRate/dspRate-th block instead keeps the reading rate at one
    // DSP-rate block's worth (~47/s) whatever the input rate, so the
    // ballistics are a property of the meter rather than of the zoom.
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
