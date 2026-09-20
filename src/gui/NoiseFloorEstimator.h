#pragma once

// The noise-floor estimator, in a header of its own so the widget and its
// tests read the SAME function rather than two copies that can drift. That is
// the reason NoiseFloorAutoAdjustGate.h next door exists (#5726), and it
// applies harder here: this function decides where m_refLevel ends up, and
// the gate that lets it run is now open for every raw-IQ radio.
//
// Two passes. Pass 1 takes the mean of every sampled bin; pass 2 averages only
// the bins at or below that mean. Signal peaks inflate the pass-1 mean and so
// exclude themselves from pass 2, leaving the flat baseline a human eye reads
// as the noise floor on the scope.
//
// IT READS EVERY BIN, WITH NO EDGE EXCLUSION. That is deliberate -- the bins
// are the radio's, not the renderer's, so a display-side edge fade does not
// and must not hide anything from it -- but it makes the estimator sensitive
// to whatever a backend does to its span edges. On a raw-IQ radio whose DDC
// rolls off, uncorrected edge bins sit tens of dB below the floor, every one
// of them lands under the pass-1 mean, and every one of them then drags pass 2
// down with it. anan_droop_noise_floor_test pins that interaction.
//
// Deliberately Qt-free and allocation-free: it runs once per displayed frame,
// and its tests must not need a QApplication.

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
