#pragma once

#include "Biquad.h"

namespace AetherSDR {

// Two Biquad sections sharing one coefficient set, with independent
// per-channel state.  Matches how the existing TX-chain DSP modules
// (ClientPudu, ClientDeEss, ClientPhaseRotator, ClientEq) use
// biquads today: a single coefficient computation per parameter
// change, separate state per L/R channel so stereo doesn't tangle.
//
// processInterleaved() consumes the typical L/R interleaved buffer
// used everywhere in AudioEngine; deinterleaving / striding for
// asymmetric-channel work is the caller's job and would use the
// per-channel `Biquad` primitive directly.
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
