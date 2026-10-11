#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace AetherSDR {

// FFT De-comb (Display panel): removes the comb a zoomed-in panadapter draws
// across an SSB voice -- the speaker's pitch harmonics, 100-250 Hz apart --
// without pulling the signal's outline down the way a mean would. The trace is
// replaced by its cepstral true envelope (core/dsp/SpectralEnvelope.h), a
// smooth curve over the harmonic peaks; being smooth, it also rounds a
// signal's steep edges over about half the width.
//
// One slider: the width in Hz, the widest harmonic spacing it removes.
// 0 is off; any positive width turns it on.
constexpr int kFftDecombMaxHz = 600;

// The envelope rides peaks, so over noise it would lift the floor toward its
// own noise peaks. Keep the floor: `env` is blended back to `orig` where it
// sits near the floor. A single unaveraged frame's noise peaks reach ~8-10 dB
// over the median, so the ramp starts at 8 dB above `floorDbm` and is fully on
// at 14 dB.
inline void decombFloorGate(std::span<const float> orig, std::span<float> env, float floorDbm)
{
    constexpr float kRampStartDb = 8.0f;
    constexpr float kRampSpanDb = 6.0f;
    const std::size_t n = std::min(orig.size(), env.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float w = std::clamp((env[i] - floorDbm - kRampStartDb) / kRampSpanDb, 0.0f, 1.0f);
        env[i] = orig[i] + w * (env[i] - orig[i]);
    }
}

} // namespace AetherSDR
