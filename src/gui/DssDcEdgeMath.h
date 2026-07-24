#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR::DssDcEdgeMath {

inline bool viewStartsAtDc(double centerMhz, double bandwidthMhz)
{
    if (!std::isfinite(centerMhz)
        || !std::isfinite(bandwidthMhz)
        || bandwidthMhz <= 0.0) {
        return false;
    }

    const double lowMhz = centerMhz - bandwidthMhz * 0.5;
    const double toleranceMhz = std::max(1.0e-9, bandwidthMhz * 1.0e-9);
    return lowMhz <= toleranceMhz;
}

// FLEX firmware 4.2.18 returns a short, steep leading spike when an FFT view
// begins exactly at DC. In 2D it is only a few edge bins, but 3D Stacked Trace
// repeats that edge through its perspective history and turns it into a large
// diagonal comb. Replace only a contiguous leading outlier with the nearby
// noise baseline; the source FFT used by the 2D trace remains untouched.
inline QVector<float> flattenLeadingSpike(const QVector<float>& binsDbm)
{
    const int count = binsDbm.size();
    if (count < 16) {
        return binsDbm;
    }

    // Bins we may replace at the head. Compute this first so the noise-baseline
    // window can start past it — the median must never be sampled from the spike
    // itself, or a longer-than-expected spike would inflate the baseline and
    // under-repair (#4413 review).
    const int candidateBins = std::clamp((count + 99) / 100, 4, 32);
    const int referenceStart = std::max(8, candidateBins);
    const int referenceEnd = std::clamp(count / 20, referenceStart + 8, 128);
    QVector<float> referenceBins;
    referenceBins.reserve(referenceEnd - referenceStart);
    for (int i = referenceStart; i < referenceEnd; ++i) {
        if (std::isfinite(binsDbm[i])) {
            referenceBins.append(binsDbm[i]);
        }
    }
    if (referenceBins.isEmpty()) {
        return binsDbm;
    }

    const int middle = referenceBins.size() / 2;
    std::nth_element(referenceBins.begin(),
                     referenceBins.begin() + middle,
                     referenceBins.end());
    const float referenceDbm = referenceBins[middle];
    constexpr float kOutlierThresholdDb = 6.0f;
    if (!std::isfinite(binsDbm[0])
        || binsDbm[0] <= referenceDbm + kOutlierThresholdDb) {
        return binsDbm;
    }

    int replaceCount = 0;
    while (replaceCount < candidateBins
           && std::isfinite(binsDbm[replaceCount])
           && binsDbm[replaceCount] > referenceDbm + kOutlierThresholdDb) {
        ++replaceCount;
    }
    if (replaceCount == 0) {
        return binsDbm;
    }

    QVector<float> repaired = binsDbm;
    std::fill(repaired.begin(), repaired.begin() + replaceCount, referenceDbm);
    return repaired;
}

} // namespace AetherSDR::DssDcEdgeMath
