#pragma once

#include <algorithm>
#include <cmath>

// The waterfall "NB Blank" impulse test (#277), as a pure function.
//
// SpectrumWidget::updateWaterfallRow keeps a ring of the last 32 row means and
// replaces a row whose mean stands out above the ring's mean (the baseline)
// with the last good row. The decision lives here because SpectrumWidget links
// into no test.
//
// THE DEFECT THIS FIXES. A row arrives in one of two units and the test knew
// only one of them:
//
//   * A Flex sends waterfall TILES: int16(raw) / 128, about 96..120 on HF.
//     Positive, so "mean > baseline * threshold" is a meaningful ratio.
//   * A radio whose spectrum is computed on this host (HL2, ANAN, RTL-SDR)
//     has no waterfall plane. RadioModel::onBackendSpectrumFrame reuses the
//     pan frame as the row, so the row is dBm: negative numbers.
//
// The test carries a `baseline > 0` guard, which a dBm baseline never passes:
// the toggle was enabled, persisted and did nothing. Dropping the guard is not
// the fix. With a baseline of -147 dBm and the default 1.15, baseline *
// threshold is -169, every ordinary row is above it, every row is replaced by
// the last good one and the waterfall stands still.
//
// The unit is not guessed from the sign of the data. It is the declared
// capability RadioCapabilities::panBinsAbsolute(), which SpectrumWidget
// already holds as m_panBinsAbsolute: true exactly where the bins are
// host-computed absolute levels and the row is therefore the pan frame.

namespace AetherSDR::WaterfallImpulseBlanker {

enum class RowKind {
    TileIntensity,  // native waterfall tile, int16(raw) / 128
    Dbm,            // host-computed pan frame reused as the row
};

// Rows of history the ring needs before any row may be called an impulse.
inline constexpr int kMinHistoryRows = 8;

// What a rejected row may add to the ring, as a threshold value: the lowest
// setting the operator control offers. It lets a level that is here to stay
// pull the baseline up until its rows pass again, without letting one burst
// move the baseline by its full height.
inline constexpr float kRejectedRowCapThreshold = 1.05f;

// dB above the baseline per unit of (threshold - 1), for a dBm row.
//
// DERIVATION. On a tile the test `mean > baseline * t` is the same statement
// as `mean - baseline > (t - 1) * baseline`: a margin in intensity units that
// is (t - 1) times the tile's own level. The widget states that level as
// about 96..120 on HF, so one step of the 5..95 control (t = 1 + step / 100)
// is worth 0.96 to 1.2 intensity units of margin. A dBm row cannot use its own
// level that way: the zero of a dBm scale is arbitrary, the margin would be
// larger the weaker the band and would move with the LNA gain. So the dBm
// margin uses a fixed reference of 100 in place of the tile's level:
//
//     margin_dB = (t - 1) * 100        one dB per step of the control
//
// 5 dB at the lowest setting, 15 dB at the default 1.15, 95 dB at the top.
// One dB is taken as the equal of one intensity unit because the widget
// already colours both row kinds with one range width in the row's own unit
// (intensityToWaterfallLevel, 120 - 0.91 * colour gain), so the same setting
// asks for the same step in COLOUR on either radio. What one tile unit is
// worth in dB is not measured, and nothing here claims it.
//
// NOT 10 * log10(t), reading t as a power ratio. That gives 0.2 dB at the
// lowest setting and 0.6 dB at the default. A row is one FFT frame, and the
// mean of its bins in dB moves by more than that when one strong station
// starts transmitting inside a narrow span (a signal 40 dB over the floor on
// 5 % of the bins lifts the mean 2 dB). The rejected-row cap would be 0.2 dB
// per 32 rows, so every such step would hold the waterfall still for seconds.
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
    if (kind == RowKind::Dbm) {
        // A baseline that is not finite fails OPEN. A -inf in the ring (one
        // empty bin in one frame) would otherwise make every later row an
        // impulse and write -inf back, and the waterfall would never move
        // again; this way the ring refills from accepted rows.
        if (historyRows >= kMinHistoryRows && std::isfinite(baseline)
                && rowMean - baseline > dbMargin(threshold)) {
            return {true, std::min(rowMean,
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
