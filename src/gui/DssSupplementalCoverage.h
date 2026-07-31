#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace AetherSDR {

// Converts a native waterfall tile into a dBm-like fallback row for the part of
// a 3D FFT trace that lies outside its captured FFT frame. The radio's waterfall
// tile is wider than the panadapter viewport, but its intensity values are an
// arbitrary display scale rather than FFT dBm. Robust overlap quantiles align
// both its floor and span for this one row. The real FFT row always wins
// wherever it has coverage.
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
    std::vector<float> tileOverlap;
    std::vector<float> fftOverlap;
    const std::size_t sampleCapacity =
        std::min<std::size_t>(tileIntensity.size(), 256);
    tileOverlap.reserve(sampleCapacity);
    fftOverlap.reserve(sampleCapacity);
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
        tileOverlap.push_back(intensity);
        fftOverlap.push_back(dbm);
    }

    // Fewer than a handful of overlap samples cannot reject a transient carrier
    // mismatch between the asynchronous FFT and waterfall packets reliably.
    if (tileOverlap.size() < 8) {
        return supplemental;
    }
    std::sort(tileOverlap.begin(), tileOverlap.end());
    std::sort(fftOverlap.begin(), fftOverlap.end());
    const auto quantile = [](const std::vector<float>& sorted, float position) {
        const float index =
            position * static_cast<float>(sorted.size() - 1);
        const std::size_t left =
            static_cast<std::size_t>(std::floor(index));
        const std::size_t right = std::min(left + 1, sorted.size() - 1);
        const float fraction = index - static_cast<float>(left);
        return sorted[left] + (sorted[right] - sorted[left]) * fraction;
    };

    constexpr float kLowQuantile = 0.1f;
    constexpr float kHighQuantile = 0.9f;
    constexpr float kMinimumTileSpan = 0.25f;
    constexpr float kMinimumScale = 0.25f;
    constexpr float kMaximumScale = 4.0f;
    const float tileFloor = quantile(tileOverlap, kLowQuantile);
    const float fftFloor = quantile(fftOverlap, kLowQuantile);
    const float tileSpan =
        quantile(tileOverlap, kHighQuantile) - tileFloor;
    const float fftSpan =
        quantile(fftOverlap, kHighQuantile) - fftFloor;

    // A flat overlap has no usable scale information. Preserve the historical
    // one-to-one mapping in that case, while still anchoring its floor.
    const float scale = tileSpan >= kMinimumTileSpan
        ? std::clamp(fftSpan / tileSpan, kMinimumScale, kMaximumScale)
        : 1.0f;
    const float offsetDb = fftFloor - scale * tileFloor;

    const float lowDbm = std::min(fallbackDbm, ceilingDbm);
    const float highDbm = std::max(fallbackDbm, ceilingDbm);
    supplemental.reserve(tileIntensity.size());
    for (const float intensity : tileIntensity) {
        const float calibrated = std::isfinite(intensity)
            ? intensity * scale + offsetDb
            : lowDbm;
        supplemental.push_back(std::clamp(calibrated, lowDbm, highDbm));
    }
    return supplemental;
}

} // namespace AetherSDR
