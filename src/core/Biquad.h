#pragma once

// Canonical 2-pole biquad section for AetherSDR DSP modules. Coefficients per
// the Audio EQ Cookbook (https://www.w3.org/TR/audio-eq-cookbook/). Direct Form
// II Transposed; coefficients in double, per-sample state in float (the ClientEq
// layout). No synchronisation: setCoefficients() and process() are both audio
// thread; cross-thread use is the caller's job. Cascade with
// std::array/std::vector<Biquad> and `for (auto& s : stages) x = s.process(x);`.

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
    //   sampleRateHz : clamped to >= 1.0 Hz
    //   centerHz     : clamped to [1.0, sampleRateHz * 0.499] (the cookbook math
    //                  collapses at the closed endpoints)
    //   Q            : clamped to >= 0.1
    //   gainDb       : PeakingEq / LowShelf / HighShelf only
    // Does NOT touch state; call reset() for a discontinuous restart.
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

    // Block tick; `in` and `out` may alias. Bit-identical to N process(x) calls, as
    // the block-process-matches-per-sample test pins; a SIMD rewrite must keep that
    // (watch for FMA contraction or reassociation differing between the two paths).
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
