#pragma once

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// The waterfall time scale's ms-per-row: how a measured row cadence becomes the
// value the scale is drawn with. Pure arithmetic, no Qt, so it can be tested
// without a widget or a clock (tests/waterfall_time_scale_lock_test.cpp).
//
// WHY THERE IS A LOCK AT ALL. #106: the scale was redrawn from a cadence
// measured on every row, and ordinary arrival jitter made the labels jump up
// and down a line. 6cbf882c fixed that by measuring for a while and then
// holding the value still; #3104 moved the measurement to row timestamps and a
// per-rate cache but kept the hold. That purpose stands: per-row jitter must
// never reach the visible scale.
//
// WHAT WAS WRONG WITH IT. The hold was final. The visible value was taken on
// the third sample after a reset and never written again, while the running
// estimate behind it kept converging on the truth. Whatever the first rows
// measured (rows arriving back to back at start-up, a stalled GUI thread) was
// the scale for the rest of the session: observed as a "1s" label on an event
// 2.00 s old.
//
// WHAT THIS DOES. The first lock is unchanged. After it, the visible value is
// replaced only when the running estimate has SETTLED somewhere else: it
// differs from the visible value by more than kWaterfallTimeScaleDriftTolerance
// and agrees with the newest window measurement within
// kWaterfallTimeScaleSettledTolerance, for a full sample window of consecutive
// samples. Jitter never moves the smoothed estimate that far. One interrupted
// stretch of rows (a stall, a pause) does, but it sits in the sample window for
// at most one window of samples, and for the first of those the estimate is
// still catching up with it, so the run cannot complete; once the gap has left
// the window the estimate is unsettled again until it is back where it started.

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

// One sample's decision. Returns true when the visible scale should take
// estimate.msPerRow now; `lock` is advanced either way.
//
// rowsPacedByScale: the rows being measured are emitted on a timer derived
// from the visible value itself (the FFT-derived fallback and TX rows). Their
// cadence is that value rounded up to the next FFT frame, so following it
// would feed the scale its own output and walk it upward one frame at a time.
// Such rows may complete the first lock, as they always have, and never a
// re-lock.
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
