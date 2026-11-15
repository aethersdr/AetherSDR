#pragma once

// Deterministic synthetic signal generators for the TX linearity analyzer
// tests.
//
// These ARE the golden test vectors. They are checked in as a generator rather
// than as captured sample files on purpose: a generator is reviewable (a
// reviewer can see that the two-tone really is two tones and that the PA model
// really has the coefficients the assertions claim), it is exactly reproducible
// on every platform because every random draw comes from a seeded std::mt19937
// with no floating-point-order dependence, and it lets a test state its
// expected answer as a closed-form expression instead of a magic number nobody
// can re-derive. Nothing here needs a radio, so all of it runs in CI.
//
// The analytic IMD levels below are derived from the standard two-tone
// expansion of a memoryless odd-order polynomial, not taken from any
// implementation. For x = A(e1 + e2) with e_i = exp(j w_i t):
//
//   |x|^2 x = A^3 [ 3e1 + 3e2 + e1^2 e2* + e2^2 e1* ]
//   |x|^4 x = A^5 [ 10e1 + 10e2 + 5 e1^2 e2* + 5 e2^2 e1*
//                   + e1^3 e2*^2 + e2^3 e1*^2 ]
//
// so for y = a1 x + a3 |x|^2 x + a5 |x|^4 x:
//
//   fundamental amplitude = a1 A + 3 a3 A^3 + 10 a5 A^5
//   IMD3 amplitude        =        a3 A^3 +  5 a5 A^5
//   IMD5 amplitude        =                     a5 A^5
//
// (Standard result; see e.g. Cripps, "RF Power Amplifiers for Wireless
// Communications", ch. 9, or Ding et al. on memory-polynomial predistortion for
// the general form.)

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <optional>
#include <random>
#include <vector>

namespace txlin_test {

// Memory polynomial y[n] = sum_{k odd} sum_{m} a[k][m] x[n-m] |x[n-m]|^(k-1).
//
// `coeff[i][m]` is the coefficient for order 2i+1 at memory tap m. Depth 1
// (m == 0 only) is the memoryless case, which is the only one with the closed
// form above — deeper memory is supported so the generator can exercise the
// Phase 3 model fit, but its IMD levels are frequency-dependent and are not
// asserted analytically.
class MemoryPolynomialPa {
public:
    MemoryPolynomialPa(double a1, double a3, double a5)
        : m_coeff{{a1, 0.0, 0.0}, {a3, 0.0, 0.0}, {a5, 0.0, 0.0}}, m_depth(1)
    {
    }

    void setMemoryTap(int order, int tap, double value)
    {
        const std::size_t i = std::size_t((order - 1) / 2);
        m_coeff[i][std::size_t(tap)] = value;
        m_depth = std::max(m_depth, tap + 1);
    }

    [[nodiscard]] int depth() const { return m_depth; }

    [[nodiscard]] std::vector<std::complex<double>>
    apply(const std::vector<std::complex<double>>& x) const
    {
        std::vector<std::complex<double>> y(x.size(), {0.0, 0.0});
        for (std::size_t n = 0; n < x.size(); ++n) {
            for (int m = 0; m < m_depth; ++m) {
                if (n < std::size_t(m)) {
                    continue;
                }
                const std::complex<double> xm = x[n - std::size_t(m)];
                const double mag2 = std::norm(xm);
                double magPow = 1.0;
                for (std::size_t i = 0; i < 3; ++i) {
                    y[n] += m_coeff[i][std::size_t(m)] * xm * magPow;
                    magPow *= mag2;
                }
            }
        }
        return y;
    }

    // Closed-form levels for a balanced two-tone of per-tone amplitude A.
    // Disengaged when the model has memory, where no closed form exists.
    struct AnalyticLevels {
        double toneAmplitude;
        double imd3Amplitude;
        double imd5Amplitude;
        [[nodiscard]] double imd3DbcPerTone() const
        {
            return 20.0 * std::log10(std::abs(imd3Amplitude) / std::abs(toneAmplitude));
        }
        [[nodiscard]] double imd5DbcPerTone() const
        {
            return 20.0 * std::log10(std::abs(imd5Amplitude) / std::abs(toneAmplitude));
        }
    };

    [[nodiscard]] std::optional<AnalyticLevels> analytic(double a) const
    {
        if (m_depth != 1) {
            return std::nullopt;
        }
        const double a1 = m_coeff[0][0];
        const double a3 = m_coeff[1][0];
        const double a5 = m_coeff[2][0];
        const double a3p = a * a * a;
        const double a5p = a * a * a * a * a;
        AnalyticLevels out;
        out.toneAmplitude = a1 * a + 3.0 * a3 * a3p + 10.0 * a5 * a5p;
        out.imd3Amplitude = a3 * a3p + 5.0 * a5 * a5p;
        out.imd5Amplitude = a5 * a5p;
        return out;
    }

private:
    // [order index 0..2][memory tap 0..3]
    double m_coeff[3][4]{};
    int m_depth = 1;
};

// Two complex baseband tones of equal amplitude `amp`, at f1 and f2 Hz.
//
// Frequencies are NOT snapped to bin centres and callers are encouraged to keep
// them off-bin: an on-bin tone is the easy case for both the peak interpolator
// and the leakage floor, so a test that only ever uses on-bin tones passes
// while the instrument is wrong about every real signal.
inline std::vector<std::complex<double>> twoTone(std::size_t n, double sampleRate,
                                                 double f1, double f2,
                                                 double amp, double amp2 = -1.0,
                                                 double phase2 = 0.0)
{
    const double a2 = (amp2 < 0.0) ? amp : amp2;
    std::vector<std::complex<double>> x(n);
    const double w1 = 2.0 * std::numbers::pi * f1 / sampleRate;
    const double w2 = 2.0 * std::numbers::pi * f2 / sampleRate;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i);
        x[i] = amp * std::complex<double>(std::cos(w1 * t), std::sin(w1 * t))
               + a2 * std::complex<double>(std::cos(w2 * t + phase2),
                                           std::sin(w2 * t + phase2));
    }
    return x;
}

// Additive complex white Gaussian noise, `sigma` being the standard deviation
// of EACH quadrature — so the total noise power is 2*sigma^2 and, spread over
// N bins, the per-bin floor is 2*sigma^2/N. Seeded, so the "measurement is
// limited by the noise floor" test is reproducible rather than flaky.
inline void addNoise(std::vector<std::complex<double>>& x, double sigma, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> dist(0.0, sigma);
    for (std::complex<double>& s : x) {
        s += std::complex<double>(dist(rng), dist(rng));
    }
}

inline std::vector<std::complex<float>>
toFloat(const std::vector<std::complex<double>>& x, std::size_t offset, std::size_t count)
{
    std::vector<std::complex<float>> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::complex<float>(float(x[offset + i].real()), float(x[offset + i].imag()));
    }
    return out;
}

}  // namespace txlin_test
