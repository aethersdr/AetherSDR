#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace AetherSDR {

// Converts a native waterfall tile into a dBm-like fallback row for the part of
// a 3D FFT trace that lies outside its captured FFT frame. The radio's waterfall
// tile is wider than the panadapter viewport, but its intensity values use a
// different offset from FFT dBm. A robust overlap median supplies that offset
// for this one row. The real FFT row always wins wherever it has coverage.
inline std::vector<float> buildDssSupplementalCoverage(
    std::span<const float> tileIntensity,
    double tileLowMhz,
    double tileHighMhz,
    std::span<const float> fftDbm,
    double fftCenterMhz,
    double fftBandwidthMhz,
    float fallbackDbm,
    float ceilingDbm)
{
    std::vector<float> supplemental;
    if (tileIntensity.empty() || fftDbm.size() < 2
        || !std::isfinite(tileLowMhz) || !std::isfinite(tileHighMhz)
        || !std::isfinite(fftCenterMhz) || !std::isfinite(fftBandwidthMhz)
        || tileHighMhz <= tileLowMhz || fftBandwidthMhz <= 0.0) {
        return supplemental;
    }

    const double fftLowMhz = fftCenterMhz - fftBandwidthMhz * 0.5;
    const double fftHighMhz = fftCenterMhz + fftBandwidthMhz * 0.5;
    const double overlapLowMhz = std::max(tileLowMhz, fftLowMhz);
    const double overlapHighMhz = std::min(tileHighMhz, fftHighMhz);
    if (overlapHighMhz <= overlapLowMhz) {
        return supplemental;
    }

    const double tileBandwidthMhz = tileHighMhz - tileLowMhz;
    std::vector<float> offsets;
    offsets.reserve(std::min<std::size_t>(tileIntensity.size(), 256));
    const std::size_t sampleStep =
        std::max<std::size_t>(1, tileIntensity.size() / 256);
    for (std::size_t index = 0; index < tileIntensity.size();
         index += sampleStep) {
        const float intensity = tileIntensity[index];
        if (!std::isfinite(intensity)) {
            continue;
        }
        const double frequencyMhz = tileLowMhz
            + (static_cast<double>(index) + 0.5)
                / static_cast<double>(tileIntensity.size())
                * tileBandwidthMhz;
        if (frequencyMhz < overlapLowMhz || frequencyMhz > overlapHighMhz) {
            continue;
        }
        const double fftPosition = (frequencyMhz - fftLowMhz)
            / fftBandwidthMhz * static_cast<double>(fftDbm.size() - 1);
        const std::size_t left = static_cast<std::size_t>(std::clamp(
            std::floor(fftPosition), 0.0,
            static_cast<double>(fftDbm.size() - 1)));
        const std::size_t right = std::min(left + 1, fftDbm.size() - 1);
        const float fraction =
            static_cast<float>(fftPosition - static_cast<double>(left));
        const float leftDbm = fftDbm[left];
        const float rightDbm = fftDbm[right];
        if (!std::isfinite(leftDbm) || !std::isfinite(rightDbm)) {
            continue;
        }
        const float dbm = leftDbm + (rightDbm - leftDbm) * fraction;
        offsets.push_back(dbm - intensity);
    }

    // Fewer than a handful of overlap samples cannot reject a transient carrier
    // mismatch between the asynchronous FFT and waterfall packets reliably.
    if (offsets.size() < 8) {
        return supplemental;
    }
    const std::size_t middle = offsets.size() / 2;
    std::nth_element(offsets.begin(), offsets.begin() + middle, offsets.end());
    const float offsetDb = offsets[middle];

    const float lowDbm = std::min(fallbackDbm, ceilingDbm);
    const float highDbm = std::max(fallbackDbm, ceilingDbm);
    supplemental.reserve(tileIntensity.size());
    for (const float intensity : tileIntensity) {
        const float calibrated = std::isfinite(intensity)
            ? intensity + offsetDb
            : lowDbm;
        supplemental.push_back(std::clamp(calibrated, lowDbm, highDbm));
    }
    return supplemental;
}

} // namespace AetherSDR
