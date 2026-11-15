#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace AetherSDR {

// FFT Smooth slider ceiling: ±16 logical pixels, a 33 px window.
constexpr int kFftSmoothingMaxHalfWidthPx = 16;

// FFT Smooth (Display panel): display-only smoothing across frequency, the
// panadapter counterpart of the AetherRX/AetherTX EQ analyzer's Smoothing
// setting (ClientEqCurveWidget::applyFractionalOctaveSmoothing). Same
// convention: each point becomes the mean of its neighbors in LINEAR power,
// then back to dB, so the noise does not drag the trace down the way a dB
// mean would. The window is a fixed number of points rather than a fraction
// of an octave because the panadapter's frequency axis is linear.
//
// The window is ±halfWidth points, clamped (shrinking) at the edges, so a
// halfWidth of 0 returns the input unchanged. `out` may alias `in`.
//
// Summed directly rather than through a running prefix sum: a trace spans
// more than 150 dB, which is beyond double precision, so P[hi] - P[lo] taken
// after a strong carrier cancels to garbage in the noise beside it.
// `linearScratch` is caller-owned so the per-frame render path does not
// allocate.
inline void smoothFftTraceLinearPower(std::span<const float> in, std::span<float> out,
                                      int halfWidth, std::vector<double>& linearScratch)
{
    const int n = static_cast<int>(std::min(in.size(), out.size()));
    if (n <= 0) {
        return;
    }
    if (halfWidth <= 0 || n < 2) {
        std::copy_n(in.begin(), n, out.begin());
        return;
    }

    // dB -> linear power: exp(x * ln(10) / 10) == pow(10, x / 10).
    constexpr double kLn10Over10 = 0.23025850929940457;
    linearScratch.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float db = in[static_cast<std::size_t>(i)];
        linearScratch[static_cast<std::size_t>(i)] =
            std::isfinite(db) ? std::exp(static_cast<double>(db) * kLn10Over10) : 0.0;
    }

    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - halfWidth);
        const int hi = std::min(n - 1, i + halfWidth);
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j) {
            sum += linearScratch[static_cast<std::size_t>(j)];
        }
        const double mean = sum / static_cast<double>(hi - lo + 1);
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(10.0 * std::log10(std::max(mean, 1e-30)));
    }
}

} // namespace AetherSDR
