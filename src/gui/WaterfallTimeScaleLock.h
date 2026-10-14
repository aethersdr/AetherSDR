#pragma once

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// The waterfall time scale's ms-per-row, as pure arithmetic for a test. The
// visible value is locked so per-row jitter never moves the labels (#106). After
// the first lock it is replaced only when the estimate has settled elsewhere:
// more than the drift tolerance from the visible value and within the settled
// tolerance of the newest window measurement, for a full window of samples.

// Rows the window measurement spans, and the fewest it accepts.
inline constexpr int kWaterfallTimeScaleSampleRows = 24;
inline constexpr int kWaterfallTimeScaleMinSampleRows = 8;
// Samples at a rate before the visible scale first takes the estimate.
inline constexpr int kWaterfallTimeScaleSamplesBeforeLock = 3;
inline constexpr int kWaterfallTimeScaleMaxSamples = 1000;
// Weight of a new window measurement in the running estimate.
inline constexpr float kWaterfallTimeScaleSampleWeight = 0.15f;
// Re-lock: how far the estimate must sit from the visible value, how closely
// it must agree with the newest measurement, and for how many consecutive
// samples. One full sample window, so that no row the visible value was taken
// from is still being measured when it is replaced.
inline constexpr float kWaterfallTimeScaleDriftTolerance = 0.10f;
inline constexpr float kWaterfallTimeScaleSettledTolerance = 0.05f;
inline constexpr int kWaterfallTimeScaleDriftSamplesBeforeRelock =
    kWaterfallTimeScaleSampleRows;

// How many rows back the next window measurement reaches, or 0 when there are
// not yet enough rows since the last reset to measure.
inline int waterfallTimeScaleSampleSpanRows(int historyRows, int rowsSinceReset)
{
    const int spanRows = std::min({kWaterfallTimeScaleSampleRows,
                                   historyRows - 1,
                                   rowsSinceReset - 1});
    return spanRows < kWaterfallTimeScaleMinSampleRows ? 0 : spanRows;
}

struct WaterfallTimeScaleEstimate {
    float msPerRow{0.0f};
    int samples{0};
};

// Fold one window measurement into the running per-rate estimate.
inline WaterfallTimeScaleEstimate foldWaterfallTimeScaleSample(
    const WaterfallTimeScaleEstimate& previous, float measuredMsPerRow)
{
    WaterfallTimeScaleEstimate updated;
    updated.msPerRow = previous.samples > 0
        ? ((1.0f - kWaterfallTimeScaleSampleWeight) * previous.msPerRow
           + kWaterfallTimeScaleSampleWeight * measuredMsPerRow)
        : measuredMsPerRow;
    updated.samples = std::min(previous.samples + 1,
                               kWaterfallTimeScaleMaxSamples);
    return updated;
}

struct WaterfallTimeScaleLock {
    bool locked{false};
    int driftSamples{0};
};

// One sample's decision: true when the visible scale should take
// estimate.msPerRow now; `lock` is advanced either way. rowsPacedByScale rows
// (fallback and TX) are timed from the visible value itself, so they may
// complete the first lock but never a re-lock.
inline bool waterfallTimeScaleShouldAdopt(
    WaterfallTimeScaleLock& lock,
    float visibleMsPerRow,
    float measuredMsPerRow,
    const WaterfallTimeScaleEstimate& estimate,
    bool rowsPacedByScale)
{
    if (!lock.locked) {
        lock.driftSamples = 0;
        if (estimate.samples < kWaterfallTimeScaleSamplesBeforeLock) {
            return false;
        }
        lock.locked = true;
        return true;
    }

    const bool usable = !rowsPacedByScale
        && std::isfinite(visibleMsPerRow) && visibleMsPerRow > 0.0f
        && std::isfinite(measuredMsPerRow) && measuredMsPerRow > 0.0f
        && std::isfinite(estimate.msPerRow) && estimate.msPerRow > 0.0f;
    if (!usable) {
        lock.driftSamples = 0;
        return false;
    }

    const bool drifted = std::fabs(estimate.msPerRow - visibleMsPerRow)
        > kWaterfallTimeScaleDriftTolerance * visibleMsPerRow;
    const bool settled = std::fabs(measuredMsPerRow - estimate.msPerRow)
        <= kWaterfallTimeScaleSettledTolerance * estimate.msPerRow;
    if (!drifted || !settled) {
        lock.driftSamples = 0;
        return false;
    }

    if (++lock.driftSamples < kWaterfallTimeScaleDriftSamplesBeforeRelock) {
        return false;
    }
    lock.driftSamples = 0;
    return true;
}

} // namespace AetherSDR
