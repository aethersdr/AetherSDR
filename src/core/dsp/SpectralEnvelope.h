#pragma once

#include <memory>
#include <span>

namespace AetherSDR {

// Cepstral "true envelope" of a dB spectrum (Röbel & Rodet, DAFx 2005), for
// the panadapter's FFT De-comb (gui/FftTraceDecomb.h).
//
// Liftering -- a DCT of the dB trace, the coefficients finer than the cutoff
// zeroed, then the inverse -- removes any ripple whose period across the trace
// is shorter than `cutoffPoints`: a voice's pitch harmonics, whatever the
// pitch. On its own it returns the dB MEAN of the comb, below its peaks, so it
// is iterated -- A = max(X, V), V = lifter(A), over-relaxed -- until V rides
// the peaks of X.
//
// The DCT is FFTW's. A plan needs FFTW's process-global planner lock, which
// other threads hold for seconds (core/dsp/FftwPlannerLock.h); this is called
// on the UI thread every frame, so compute() only TRIES the lock and returns
// false when a new length cannot be planned yet. Plans are cached per padded
// length, and executing one takes no lock.
class SpectralEnvelope {
public:
    SpectralEnvelope();
    ~SpectralEnvelope();
    SpectralEnvelope(const SpectralEnvelope&) = delete;
    SpectralEnvelope& operator=(const SpectralEnvelope&) = delete;

    // `in` and `out` are dB, same length, and may alias. Returns false (out
    // untouched) when the plan for this length is not available this frame,
    // or when there is nothing to do (cutoffPoints < 2 or fewer than 4 points).
    bool compute(std::span<const float> in, std::span<float> out, double cutoffPoints);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
