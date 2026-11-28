#pragma once

#include <QVector>
#include <QtGlobal>

namespace AetherSDR {

// Range switch for the cross-needle power/SWR meter (#6433).
//
// Every range scales forward and reflected full scale by the same multiplier,
// so the printed SWR contours stay exact on all of them. What a range decides
// is whether the needle crossing is READABLE: the face conceals the contours'
// convergence under the lower SWR mask (docs/cross-needle-meter-math.md, D1),
// so a forward needle sitting low on a large range puts low-SWR crossings
// under the mask. A hardware meter solves this with a range switch; this class
// is that switch, driven by the measured forward power.
//
// Policy:
//   * The ceiling is the smallest range that holds the radio's maximum power
//     below kUpRangeFraction of full scale (100 W radio: 200 W; 500 W radio:
//     600 W), or the top range while an amplifier feeds the meter. The range
//     never exceeds it.
//   * Up-range immediately, to the smallest range that fits, as soon as the
//     forward power exceeds kUpRangeFraction of the current full scale. A
//     needle on its end stop reads nothing above it, so a radio at its rated
//     power (100 W into a 100 W range) moves up to the next range.
//   * Down-range only after the forward power has stayed at or below
//     kDownRangeHeadroom of a lower range's full scale for kDownRangeHoldMs.
//     The gap between the up and down fractions prevents hunting on a steady
//     carrier; the hold time keeps SSB peaks from hunting.
//   * Samples below kSignalFloorWatts carry no information (SSB pauses, CW
//     between elements, unkeyed packets) and neither start nor end a hold.
//   * The range is kept across unkey, so the next transmission starts where
//     the last one ended.
//
// Pure state: time is passed in, so the policy is testable without timers.
class CrossNeedleRangeSelector {
  public:
    static constexpr double kUpRangeFraction = 0.95;
    static constexpr double kDownRangeHeadroom = 0.90;
    static constexpr qint64 kDownRangeHoldMs = 1500;
    static constexpr double kSignalFloorWatts = 0.5;

    CrossNeedleRangeSelector();
    CrossNeedleRangeSelector(QVector<double> ladder, double forwardFullScaleWatts);

    double multiplier() const;
    double ceilingMultiplier() const;
    const QVector<double> &ladder() const { return m_ladder; }
    bool holdPending() const { return m_holdStartMs >= 0; }
    // Milliseconds until a pending down-range is due at nowMs; 0 when due or
    // when no hold is pending.
    qint64 holdRemainingMs(qint64 nowMs) const;

    // Returns true when the active range changed. Re-asserting an unchanged
    // ceiling is a no-op and leaves a pending down-range running.
    bool setCeiling(int maxWatts, bool amplifierActive);
    bool observe(double forwardWatts, qint64 nowMs);
    void cancelHold();

  private:
    int fittingIndex(double watts, double headroom) const;
    double fullScaleAt(int index) const;

    QVector<double> m_ladder;
    double m_forwardFullScaleWatts{20.0};
    int m_index{0};
    int m_ceilingIndex{0};
    bool m_observed{false};
    qint64 m_holdStartMs{-1};
    int m_holdIndex{-1};
};

} // namespace AetherSDR
