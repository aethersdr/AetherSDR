#pragma once

#include "core/dsp/SpectralEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

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

// The De-comb width in source bins, or 0 when De-comb should not run: the
// width must span at least two bins, or the spectrum cannot hold a comb at
// that spacing. Applied to the source bins, before the drawn trace is
// resampled, so the GPU and CPU paths and the device pixel ratio cannot
// change whether it runs.
inline double decombWindowPoints(double hzPerBin, int widthHz)
{
    if (widthHz <= 0 || !(hzPerBin > 0.0)) {
        return 0.0;
    }
    const double points = static_cast<double>(widthHz) / hzPerBin;
    return points >= 2.0 ? points : 0.0;
}

struct DecombScratch {
    std::vector<float> sorted;
    std::vector<float> clipped;
    std::vector<float> envelope;
};

// De-comb `trace` (dB) in place. The envelope rides peaks, so over noise it
// would ride the noise peaks and lift the floor several dB. So the input is
// first clipped from below at kClipDb over the trace's 20th percentile -- a
// floor reference that holds while a signal fills up to ~80% of the span;
// unaveraged noise crosses the clip ~0.1% of the time -- and the envelope is
// blended in only where it rises clear of the clip. Over noise the trace is
// left as it was; over a voice more than ~8 dB above the noise median the
// comb is filled. Non-finite points (no data) are left as they are.
// Returns false, trace untouched, when the envelope is unavailable this frame.
inline bool decombTrace(std::span<float> trace, double windowPoints, SpectralEnvelope& env,
                        DecombScratch& s)
{
    constexpr float kClipDb = 12.0f;
    constexpr float kRampStartDb = 1.0f;
    constexpr float kRampSpanDb = 5.0f;

    s.sorted.clear();
    for (const float v : trace) {
        if (std::isfinite(v)) {
            s.sorted.push_back(v);
        }
    }
    if (s.sorted.size() < 4) {
        return false;
    }
    const auto k = static_cast<std::ptrdiff_t>((s.sorted.size() - 1) / 5);
    std::nth_element(s.sorted.begin(), s.sorted.begin() + k, s.sorted.end());
    const float clip = s.sorted[static_cast<std::size_t>(k)] + kClipDb;

    s.clipped.resize(trace.size());
    for (std::size_t i = 0; i < trace.size(); ++i) {
        s.clipped[i] = std::isfinite(trace[i]) ? std::max(trace[i], clip) : clip;
    }
    s.envelope.resize(trace.size());
    if (!env.compute(s.clipped, s.envelope, windowPoints)) {
        return false;
    }
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (!std::isfinite(trace[i])) {
            continue;
        }
        const float w =
            std::clamp((s.envelope[i] - clip - kRampStartDb) / kRampSpanDb, 0.0f, 1.0f);
        trace[i] += w * (s.envelope[i] - trace[i]);
    }
    return true;
}

} // namespace AetherSDR
