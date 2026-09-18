#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace AetherSDR::certmath {

// The measurement primitives behind RadioCertification, in a header of their own
// for ONE reason: both of the measurement bugs this branch has fixed landed
// here, and neither was reachable by a test while these lived in an anonymous
// namespace inside the .cpp.
//
// Both bugs were the same shape — the right correlator asked about the wrong
// frequency, because the sample rate was assumed rather than read. Nothing in
// tonePower() can detect that; it faithfully reports the power at whatever bin
// the caller's `fs` implies. So the tests that matter here pin the property the
// CALLER depends on: that a wrong `fs` moves the probe off the tone, which is
// what makes "read the rate from the capture" a correctness requirement rather
// than a style preference.

inline constexpr double kPi = 3.14159265358979323846;

// Correlate a real audio buffer against one frequency. Used instead of a full
// FFT because we are asking one question about one known frequency.
//
// `fs` MUST be the rate the samples were actually captured at. Passing a
// constant here is the defect documented in docs/HERMES.md 1.9: the probe lands on
// hz*(fsActual/fs), reads the noise floor, and the caller concludes "no signal"
// from what is really "looked in the wrong place".
inline double tonePower(const std::vector<float>& mono, double hz, double fs)
{
    if (mono.empty() || fs <= 0.0)
        return 0.0;
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * kPi * hz / fs;
    for (std::size_t n = 0; n < mono.size(); ++n) {
        const double ph = w * static_cast<double>(n);
        acc += static_cast<double>(mono[n])
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(mono.size());
}

inline double rms(const std::vector<float>& mono)
{
    if (mono.empty())
        return 0.0;
    double acc = 0.0;
    for (const float v : mono)
        acc += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(acc / static_cast<double>(mono.size()));
}

inline double db(double v) { return 20.0 * std::log10(std::max(1e-12, v)); }

// Strongest bin within +/- `spanHz` of `hz`. Use this instead of tonePower()
// whenever the tone's exact frequency is not under our control.
//
// tonePower() integrates coherently over the whole buffer, so a 1.5 s capture
// is a ~0.67 Hz bin. That is the right thing for our own test tone, whose
// frequency we set. It is the WRONG thing for an off-air reference: WWV is
// exact, but OUR dial is not, and a 1 ppm oscillator error at 10 MHz moves the
// carrier ~10 Hz — fifteen bins away. The tone then reads as the noise floor in
// every mode at once, which looks exactly like "the receiver is deaf" rather
// than "the probe missed".
//
// That failure is indistinguishable from the §1.9 wrong-rate bug from the
// outside, and it would be read the same wrong way.
inline double tonePowerNear(const std::vector<float>& mono, double hz,
                            double fs, double spanHz, double stepHz = 1.0)
{
    if (mono.empty() || fs <= 0.0 || stepHz <= 0.0)
        return 0.0;
    double best = 0.0;
    for (double f = hz - spanHz; f <= hz + spanHz; f += stepHz)
        best = std::max(best, tonePower(mono, f, fs));
    return best;
}

}  // namespace AetherSDR::certmath
