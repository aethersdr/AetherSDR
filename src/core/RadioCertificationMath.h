#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace AetherSDR::certmath {

// Measurement primitives for RadioCertification, in a header so they're
// testable. tonePower() reports power at whatever bin the caller's `fs`
// implies; tests pin that a wrong `fs` moves the probe off the tone, which is
// why callers must read the rate from the capture.

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

// Strongest bin within +/- `spanHz` of `hz`; use instead of tonePower() when
// the tone's exact frequency isn't ours. tonePower() is coherent over the whole
// buffer (1.5 s ≈ 0.67 Hz bins), so a 1 ppm dial error at 10 MHz (~10 Hz)
// puts an off-air reference like WWV fifteen bins away and it reads as noise.
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
