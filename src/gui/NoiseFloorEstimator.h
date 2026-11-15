#pragma once

// Noise-floor estimator, shared by the widget and its tests (#5726); it sets
// m_refLevel for every raw-IQ radio. Two passes: mean of all bins, then mean of
// bins at or below it, so peaks exclude themselves. Reads every bin with no
// edge exclusion, so uncorrected DDC edge roll-off drags pass 2 down
// (anan_droop_noise_floor_test). Qt-free and allocation-free: runs per frame.

#include <cmath>
#include <cstddef>
#include <span>

namespace AetherSDR {

[[nodiscard]] inline float estimateNoiseFloorDbm(std::span<const float> bins) noexcept
{
    if (bins.empty())
        return -1000.0f;

    // Stride-sample to cap work at ~512 reads even on very wide pans.
    const std::size_t stride =
        std::max<std::size_t>(1, static_cast<std::size_t>(bins.size() / 512));
    float sum = 0.0f;
    int count = 0;
    for (std::size_t i = 0; i < bins.size(); i += stride) {
        const float v = bins[i];
        if (std::isfinite(v)) { sum += v; ++count; }
    }
    if (count <= 0)
        return -1000.0f;

    const float mean = sum / static_cast<float>(count);
    float baselineSum = 0.0f;
    int baselineCount = 0;
    for (std::size_t i = 0; i < bins.size(); i += stride) {
        const float v = bins[i];
        if (std::isfinite(v) && v <= mean) { baselineSum += v; ++baselineCount; }
    }
    return (baselineCount > 0) ? baselineSum / static_cast<float>(baselineCount) : mean;
}

}  // namespace AetherSDR
