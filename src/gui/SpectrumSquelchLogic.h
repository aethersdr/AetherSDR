#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace AetherSDR::SpectrumSquelchLogic {
// The display's Auto SQL estimator. Its floor and threshold stay in the
// backend's declared bin units; scale conversion happens only at the boundary.
inline std::optional<int> suggest(std::span<const float> bins, float& floor,
    double referenceDb, double stepDb, int marginDb)
{
    if (!std::isfinite(referenceDb) || !std::isfinite(stepDb) || stepDb <= 0) { return {}; }
    double sum = 0;
    int count = 0;
    for (std::size_t i = 0; i < bins.size(); i += 4) {
        if (std::isfinite(bins[i])) { sum += bins[i]; ++count; }
    }
    if (!count) { return {}; }
    const double mean = sum / count;
    sum = 0; count = 0;
    for (std::size_t i = 0; i < bins.size(); i += 4) {
        if (std::isfinite(bins[i]) && bins[i] <= mean) { sum += bins[i]; ++count; }
    }
    const double frameFloor = count ? sum / count : mean;
    floor = (!std::isfinite(floor) || floor <= -500.0f)
        ? frameFloor : 0.1 * frameFloor + 0.9 * floor;
    return static_cast<int>(std::clamp((floor + std::clamp(marginDb, 5, 20)
        - referenceDb) / stepDb + 0.5, 1.0, 100.0));
}
} // namespace AetherSDR::SpectrumSquelchLogic
