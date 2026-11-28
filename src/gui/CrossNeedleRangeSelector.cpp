#include "CrossNeedleRangeSelector.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {

namespace {

// 1-2-5 ladder plus 600 W (#6433): no step is wider than 2.5x, so a settled
// forward needle sits at or above kDownRangeHeadroom / 2.5 = 36% of full
// scale. There an SWR 1.5 crossing clears the mask comfortably and 1.35 still
// clears it. A 1-3-10 ladder settles as low as 27%, where 1.35 is hidden. The
// 600 W step gives a 500 W radio a top range it fills to 83% instead of 50%.
QVector<double> defaultLadder() {
    return {1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 50.0, 100.0};
}

QVector<double> sanitizedLadder(QVector<double> ladder) {
    ladder.erase(std::remove_if(ladder.begin(), ladder.end(),
                                [](double m) { return !std::isfinite(m) || m <= 0.0; }),
                 ladder.end());
    std::sort(ladder.begin(), ladder.end());
    ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
    return ladder.isEmpty() ? defaultLadder() : ladder;
}

} // namespace

CrossNeedleRangeSelector::CrossNeedleRangeSelector()
    : CrossNeedleRangeSelector(defaultLadder(), 20.0) {}

CrossNeedleRangeSelector::CrossNeedleRangeSelector(QVector<double> ladder,
                                                   double forwardFullScaleWatts)
    : m_ladder(sanitizedLadder(std::move(ladder))),
      m_forwardFullScaleWatts(forwardFullScaleWatts > 0.0 ? forwardFullScaleWatts : 20.0) {
    m_ceilingIndex = static_cast<int>(m_ladder.size()) - 1;
    m_index = m_ceilingIndex;
}

double CrossNeedleRangeSelector::multiplier() const { return m_ladder[m_index]; }

double CrossNeedleRangeSelector::ceilingMultiplier() const { return m_ladder[m_ceilingIndex]; }

double CrossNeedleRangeSelector::fullScaleAt(int index) const {
    return m_forwardFullScaleWatts * m_ladder[index];
}

int CrossNeedleRangeSelector::fittingIndex(double watts, double headroom) const {
    for (int i = 0; i <= m_ceilingIndex; ++i) {
        if (headroom * fullScaleAt(i) >= watts) {
            return i;
        }
    }
    return m_ceilingIndex;
}

qint64 CrossNeedleRangeSelector::holdRemainingMs(qint64 nowMs) const {
    if (m_holdStartMs < 0) {
        return 0;
    }
    return std::max<qint64>(0, m_holdStartMs + kDownRangeHoldMs - nowMs);
}

void CrossNeedleRangeSelector::cancelHold() {
    m_holdStartMs = -1;
    m_holdIndex = -1;
}

bool CrossNeedleRangeSelector::setCeiling(int maxWatts, bool amplifierActive) {
    const int last = static_cast<int>(m_ladder.size()) - 1;
    int ceiling = last;
    if (!amplifierActive) {
        for (int i = 0; i <= last; ++i) {
            // Same headroom as up-ranging: the radio's rated power must not
            // land on the end stop of its top range (100 W radio -> 200 W).
            if (kUpRangeFraction * fullScaleAt(i) >= static_cast<double>(maxWatts)) {
                ceiling = i;
                break;
            }
        }
    }
    // The caller re-asserts the ceiling on every amplifier status poll; only a
    // real change may disturb the range or a pending down-range.
    if (ceiling == m_ceilingIndex) {
        return false;
    }
    m_ceilingIndex = ceiling;
    cancelHold();

    const int previous = m_index;
    // Before the first transmission there is no measured power to range on,
    // so the idle face shows the radio's own full range.
    m_index = m_observed ? std::min(m_index, m_ceilingIndex) : m_ceilingIndex;
    return m_index != previous;
}

bool CrossNeedleRangeSelector::observe(double forwardWatts, qint64 nowMs) {
    if (!std::isfinite(forwardWatts) || forwardWatts < kSignalFloorWatts) {
        return false;
    }
    m_observed = true;

    if (forwardWatts > kUpRangeFraction * fullScaleAt(m_index)) {
        cancelHold();
        const int up = fittingIndex(forwardWatts, kUpRangeFraction);
        if (up == m_index) {
            return false; // already at the ceiling: the needle pins, as before
        }
        m_index = up;
        return true;
    }

    const int down = fittingIndex(forwardWatts, kDownRangeHeadroom);
    if (down >= m_index) {
        // This sample needs the current range: any pending down-range is void.
        cancelHold();
        return false;
    }

    if (m_holdStartMs < 0) {
        m_holdStartMs = nowMs;
        m_holdIndex = down;
    } else {
        // The hold lands on the range the LARGEST sample in it still fits.
        m_holdIndex = std::max(m_holdIndex, down);
    }
    if (nowMs - m_holdStartMs < kDownRangeHoldMs) {
        return false;
    }
    m_index = m_holdIndex;
    cancelHold();
    return true;
}

} // namespace AetherSDR
