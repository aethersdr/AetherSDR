#pragma once

#include "Biquad.h"

namespace AetherSDR {

// Two Biquads sharing one coefficient set with independent L/R state.
// processInterleaved() takes the L/R interleaved buffers AudioEngine uses;
// asymmetric channel work should use Biquad directly.
class StereoBiquad {
public:
    void setCoefficients(Biquad::Type type, double sampleRateHz,
                         double centerHz, double Q,
                         double gainDb = 0.0) noexcept;

    // Process `frames` stereo frames in place.  Buffer length is
    // frames * 2 floats.  Mono callers should use `Biquad` directly.
    void processInterleaved(float* lr, int frames) noexcept;

    // Zero per-channel state without touching coefficients.
    void reset() noexcept;

private:
    Biquad m_left;
    Biquad m_right;
};

} // namespace AetherSDR
