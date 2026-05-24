#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.283185307179586476;
constexpr double kMinQ  = 0.1;

struct RawCoeffs {
    double b0{1.0}, b1{0.0}, b2{0.0};
    double a0{1.0}, a1{0.0}, a2{0.0};
};

RawCoeffs cookbookCoefficients(Biquad::Type type, double sampleRateHz,
                               double centerHz, double Q, double gainDb)
{
    // Clamp inputs to the safe envelope cookbook formulas assume:
    //   fs > 0, 0 < fc < fs/2, Q >= kMinQ.  Outside these ranges the
    //   coefficients diverge (sin(0) = 0 in alpha; cos(π) = -1
    //   collapses the denominator).  Clamp rather than NaN-out
    //   because audio-thread callers cannot recover from a NaN'd
    //   coefficient set without a reset.
    const double fs = std::max(1.0, sampleRateHz);
    const double fc = std::clamp(centerHz, 1.0, fs * 0.499);
    const double q  = std::max(kMinQ, Q);
    const double A  = std::pow(10.0, gainDb / 40.0);

    const double w0    = kTwoPi * fc / fs;
    const double cosw  = std::cos(w0);
    const double sinw  = std::sin(w0);
    const double alpha = sinw / (2.0 * q);

    RawCoeffs c;
    switch (type) {
    case Biquad::Type::LowPass:
        c.b0 = (1.0 - cosw) * 0.5;
        c.b1 =  1.0 - cosw;
        c.b2 = (1.0 - cosw) * 0.5;
        c.a0 =  1.0 + alpha;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha;
        break;
    case Biquad::Type::HighPass:
        c.b0 =  (1.0 + cosw) * 0.5;
        c.b1 = -(1.0 + cosw);
        c.b2 =  (1.0 + cosw) * 0.5;
        c.a0 =   1.0 + alpha;
        c.a1 =  -2.0 * cosw;
        c.a2 =   1.0 - alpha;
        break;
    case Biquad::Type::BandPassConstQ:
        // Constant skirt gain; peak gain = Q.
        c.b0 =  q * alpha;
        c.b1 =  0.0;
        c.b2 = -q * alpha;
        c.a0 =  1.0 + alpha;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha;
        break;
    case Biquad::Type::BandPassPeak:
        // Constant 0 dB peak gain.
        c.b0 =  alpha;
        c.b1 =  0.0;
        c.b2 = -alpha;
        c.a0 =  1.0 + alpha;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha;
        break;
    case Biquad::Type::Notch:
        c.b0 =  1.0;
        c.b1 = -2.0 * cosw;
        c.b2 =  1.0;
        c.a0 =  1.0 + alpha;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha;
        break;
    case Biquad::Type::AllPass:
        c.b0 =  1.0 - alpha;
        c.b1 = -2.0 * cosw;
        c.b2 =  1.0 + alpha;
        c.a0 =  1.0 + alpha;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha;
        break;
    case Biquad::Type::PeakingEq:
        c.b0 =  1.0 + alpha * A;
        c.b1 = -2.0 * cosw;
        c.b2 =  1.0 - alpha * A;
        c.a0 =  1.0 + alpha / A;
        c.a1 = -2.0 * cosw;
        c.a2 =  1.0 - alpha / A;
        break;
    case Biquad::Type::LowShelf: {
        const double sqrtA = std::sqrt(A);
        const double k     = 2.0 * sqrtA * alpha;
        c.b0 =        A * ((A + 1.0) - (A - 1.0) * cosw + k);
        c.b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
        c.b2 =        A * ((A + 1.0) - (A - 1.0) * cosw - k);
        c.a0 =             (A + 1.0) + (A - 1.0) * cosw + k;
        c.a1 =     -2.0 * ((A - 1.0) + (A + 1.0) * cosw);
        c.a2 =             (A + 1.0) + (A - 1.0) * cosw - k;
        break;
    }
    case Biquad::Type::HighShelf: {
        const double sqrtA = std::sqrt(A);
        const double k     = 2.0 * sqrtA * alpha;
        c.b0 =        A * ((A + 1.0) + (A - 1.0) * cosw + k);
        c.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
        c.b2 =        A * ((A + 1.0) + (A - 1.0) * cosw - k);
        c.a0 =             (A + 1.0) - (A - 1.0) * cosw + k;
        c.a1 =      2.0 * ((A - 1.0) - (A + 1.0) * cosw);
        c.a2 =             (A + 1.0) - (A - 1.0) * cosw - k;
        break;
    }
    }
    return c;
}

} // namespace

void Biquad::setCoefficients(Type type, double sampleRateHz, double centerHz,
                             double Q, double gainDb) noexcept
{
    const RawCoeffs c = cookbookCoefficients(type, sampleRateHz,
                                             centerHz, Q, gainDb);
    const double invA0 = 1.0 / c.a0;
    m_b0 = c.b0 * invA0;
    m_b1 = c.b1 * invA0;
    m_b2 = c.b2 * invA0;
    m_a1 = c.a1 * invA0;
    m_a2 = c.a2 * invA0;
}

void Biquad::process(const float* in, float* out, int n) noexcept
{
    // Hold coefficients in float locals so the inner loop avoids
    // repeated narrowing.  Bit-identical to N successive process(x)
    // calls because process(x) itself narrows on every reference.
    const float b0 = static_cast<float>(m_b0);
    const float b1 = static_cast<float>(m_b1);
    const float b2 = static_cast<float>(m_b2);
    const float a1 = static_cast<float>(m_a1);
    const float a2 = static_cast<float>(m_a2);
    float z1 = m_z1;
    float z2 = m_z2;
    for (int i = 0; i < n; ++i) {
        const float x = in[i];
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        out[i] = y;
    }
    m_z1 = z1;
    m_z2 = z2;
}

double Biquad::magnitudeDbAt(double probeHz, double sampleRateHz) const noexcept
{
    // Evaluate |H(e^jw)|^2 = |num|^2 / |den|^2 at w = 2π * probe / fs.
    // For z = e^{jw}, z^{-1} = cos(w) - j sin(w) and z^{-2} =
    // cos(2w) - j sin(2w), so the numerator and denominator each
    // decompose into a (real, imag) pair below.
    const double fs = std::max(1.0, sampleRateHz);
    const double w  = kTwoPi * std::clamp(probeHz, 0.0, fs * 0.5) / fs;
    const double c1 = std::cos(w),     s1 = std::sin(w);
    const double c2 = std::cos(2.0*w), s2 = std::sin(2.0*w);

    const double numRe = m_b0 + m_b1 * c1 + m_b2 * c2;
    const double numIm =       -m_b1 * s1 - m_b2 * s2;
    const double denRe = 1.0 + m_a1 * c1 + m_a2 * c2;
    const double denIm =      -m_a1 * s1 - m_a2 * s2;

    const double num2 = numRe * numRe + numIm * numIm;
    const double den2 = denRe * denRe + denIm * denIm;
    if (den2 < 1e-40) return -240.0;
    const double magSq = num2 / den2;
    if (magSq < 1e-24) return -240.0;
    return 10.0 * std::log10(magSq);
}

double Biquad::magnitudeDbAt(Type type, double sampleRateHz,
                             double centerHz, double Q, double gainDb,
                             double probeHz) noexcept
{
    Biquad probe;
    probe.setCoefficients(type, sampleRateHz, centerHz, Q, gainDb);
    return probe.magnitudeDbAt(probeHz, sampleRateHz);
}

} // namespace AetherSDR
