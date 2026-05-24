#pragma once

// Canonical 2-pole biquad section for AetherSDR DSP modules.
//
// Coefficient computation follows the Audio EQ Cookbook (Robert
// Bristow-Johnson):
//   https://www.w3.org/TR/audio-eq-cookbook/
//
// Topology: Direct Form II Transposed.  Chosen because it produces
// the cleanest single-precision state behaviour of the four standard
// biquad forms and matches the existing high-quality reference
// implementation in ClientEq.  Coefficients are stored in `double`
// for numerical stability across the full audio band; per-sample
// state is `float` to keep the inner loop lean — this matches the
// long-standing ClientEq layout and is the precision split the
// scattered DESS / Pudu implementations should have used.
//
// Thread model: no synchronisation.  setCoefficients() is intended
// to be called from the audio thread (typically once per block when
// a parameter version counter changes); process() is also audio-
// thread.  Holding a Biquad across threads is the caller's
// responsibility — wrap it with whatever atomics + version counter
// the surrounding DSP module already uses.
//
// Cascade pattern: for higher-order filters that need N biquad stages
// in series (DESS bandpass cascade, PhaseRotator's 4-stage all-pass,
// EqApplet's per-band sections), use std::array<Biquad, N> for fixed
// stage counts or std::vector<Biquad> for runtime-variable counts and
// iterate explicitly:
//
//   for (auto& stage : stages) sample = stage.process(sample);
//
// No dedicated BiquadCascade class — the std::array/std::vector idiom
// is simpler than a wrapper and lets each consumer choose its own
// stage-count semantics (compile-time vs. runtime, reset granularity,
// per-stage coefficient sets).

namespace AetherSDR {

class Biquad {
public:
    enum class Type {
        LowPass,
        HighPass,
        BandPassConstQ,    // cookbook BPF variant 1: peak gain = Q (constant skirt)
        BandPassPeak,      // cookbook BPF variant 2: constant 0 dB peak gain
        Notch,             // cookbook notch (band-reject)
        AllPass,
        PeakingEq,
        LowShelf,
        HighShelf,
    };

    // Compute and store coefficients per Audio EQ Cookbook.
    //   sampleRateHz : audio thread sample rate, clamped to >= 1.0 Hz
    //   centerHz     : corner / centre / shelf-midpoint, clamped to
    //                  [1.0, sampleRateHz * 0.499].  Avoids the
    //                  sin(0)=0 / cos(π)=-1 collapse the cookbook math
    //                  exhibits at the closed endpoints.
    //   Q            : resonance / bandwidth control, clamped to >= 0.1
    //   gainDb       : used only by PeakingEq / LowShelf / HighShelf
    // Does NOT touch state — call reset() separately if a discontinuous
    // restart is required.
    void setCoefficients(Type type, double sampleRateHz, double centerHz,
                         double Q, double gainDb = 0.0) noexcept;

    // Single-sample tick.  Per-sample loops can inline this trivially.
    float process(float x) noexcept
    {
        const float y  = static_cast<float>(m_b0) * x
                       + m_z1;
        m_z1 = static_cast<float>(m_b1) * x
             - static_cast<float>(m_a1) * y
             + m_z2;
        m_z2 = static_cast<float>(m_b2) * x
             - static_cast<float>(m_a2) * y;
        return y;
    }

    // Block tick.  `in` and `out` may alias.  Bit-identical to N
    // successive process(x) calls, which is the property the
    // block-process-matches-per-sample test pins down so future
    // SIMD vectorisation has a known reference.
    //
    // Caveat for future SIMD work: the bit-identity guarantee assumes
    // the compiler doesn't introduce FMA contraction or reassociation
    // asymmetrically between this path and the per-sample inline
    // process(x) above.  Block-form locals pre-narrow to float, which
    // can tempt vectorisation in a way the per-call narrowing doesn't.
    // Any SIMD rewrite of this function must preserve the test's
    // bit-identity assertion — break it and the test fails loudly.
    void process(const float* in, float* out, int n) noexcept;

    // Zero state (z1, z2) without touching coefficients.  After
    // reset() the section behaves as if it has just started — useful
    // between discontinuous bursts (TX on/off, sample-rate change).
    void reset() noexcept { m_z1 = 0.0f; m_z2 = 0.0f; }

    // Digital magnitude response of this section's currently-configured
    // coefficients at probeHz, expressed in dB.  Useful for response-
    // curve drawers that want to query a configured biquad without
    // re-deriving the cookbook math.
    double magnitudeDbAt(double probeHz, double sampleRateHz) const noexcept;

    // Stateless variant: compute the digital magnitude response of a
    // biquad with the given parameters at probeHz, without needing to
    // construct an instance.  Useful for response-curve widgets that
    // sweep probe frequencies over a hypothetical filter design.
    static double magnitudeDbAt(Type type, double sampleRateHz,
                                double centerHz, double Q, double gainDb,
                                double probeHz) noexcept;

private:
    // Direct Form II Transposed:
    //   y[n] = b0*x[n] + z1
    //   z1   = b1*x[n] - a1*y[n] + z2
    //   z2   = b2*x[n] - a2*y[n]
    // Coefficients are pre-normalised by a0 in setCoefficients() so the
    // tick form above can omit a0.
    double m_b0{1.0}, m_b1{0.0}, m_b2{0.0};
    double m_a1{0.0}, m_a2{0.0};
    float  m_z1{0.0f}, m_z2{0.0f};
};

} // namespace AetherSDR
