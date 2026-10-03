#pragma once

#include <algorithm>
#include <cmath>

// The waterfall "NB Blank" impulse test (#277), as a pure function, since
// SpectrumWidget links into no test. On a Flex tile (positive intensity) it is
// a ratio over the ring's mean; on an absolute dB row (dBFS under a dBm label,
// negative) it is a dB margin. The unit is the declared capability
// RadioCapabilities::panBinsAbsolute(), never the sign of the data.

namespace AetherSDR::WaterfallImpulseBlanker {

enum class RowKind {
    TileIntensity,  // native waterfall tile, int16(raw) / 128
    AbsoluteDb,     // host-computed pan frame reused as the row
};

// Row means the baseline ring holds; SpectrumWidget asserts its ring is this.
// The ring holds one unit at a time: the row kind changes only across a
// disconnect, and SpectrumWidget::clearDisplay empties the ring there.
inline constexpr int kRingRows = 32;

// Rows of history the ring needs before any row may be called an impulse.
inline constexpr int kMinHistoryRows = 8;

// What a rejected row may add to the ring, as a threshold value: the lowest
// setting the operator control offers. It lets a level that is here to stay
// pull the baseline up until its rows pass again, without letting one burst
// move the baseline by its full height.
inline constexpr float kRejectedRowCapThreshold = 1.05f;

// dB above the baseline per unit of (threshold - 1) on an absolute row: one dB
// per step of the 5..95 control (5 dB lowest, 15 dB at the default 1.15). One
// dB is taken as one tile intensity unit, since both row kinds share one colour
// range width; 10*log10(t) would be a fraction of a dB, below the movement of
// an ordinary row's mean, and would freeze the waterfall.
inline constexpr float kDbPerThresholdUnit = 100.0f;

inline float dbMargin(float threshold)
{
    return (threshold - 1.0f) * kDbPerThresholdUnit;
}

struct Decision {
    bool impulse;     // replace this row with the last good one
    float ringValue;  // what this row contributes to the baseline ring
};

// historyRows: rows already in the ring. baseline: their mean. rowMean: the
// mean of the incoming row's bins. threshold: the operator's setting, 1.05 to
// 2.0. baseline and rowMean are in the unit `kind` names.
inline Decision decide(RowKind kind, int historyRows, float baseline,
                       float rowMean, float threshold)
{
    if (kind == RowKind::AbsoluteDb) {
        // A baseline that is not finite fails OPEN. A -inf in the ring (one
        // empty bin in one frame) would otherwise make every later row an
        // impulse and write -inf back, and the waterfall would never move
        // again; this way the ring refills from accepted rows.
        if (historyRows >= kMinHistoryRows && std::isfinite(baseline)
                && rowMean - baseline > dbMargin(threshold)) {
            return {true,
                    std::min(rowMean,
                             baseline + dbMargin(kRejectedRowCapThreshold))};
        }
        return {false, rowMean};
    }

    // Tile intensity: the test and the cap exactly as #277 wrote them.
    if (historyRows >= kMinHistoryRows && baseline > 0.0f
            && rowMean > baseline * threshold) {
        return {true, std::min(rowMean, baseline * kRejectedRowCapThreshold)};
    }
    return {false, rowMean};
}

} // namespace AetherSDR::WaterfallImpulseBlanker
