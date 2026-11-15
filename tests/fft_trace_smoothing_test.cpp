#include "gui/FftTraceSmoothing.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expectNear(const char* what, float actual, float expected, float tol = 1e-3f)
{
    if (!(std::fabs(actual - expected) <= tol)) {
        std::fprintf(stderr, "FAIL: %s: expected %.4f, got %.4f\n", what, expected, actual);
        ++failures;
    }
}

std::vector<float> smooth(const std::vector<float>& in, int halfWidth)
{
    std::vector<float> out(in.size());
    std::vector<double> scratch;
    AetherSDR::smoothFftTraceLinearPower(in, out, halfWidth, scratch);
    return out;
}

} // namespace

int main()
{
    // Off is bit-identical: FFT Smooth at 0 must be exactly today's trace.
    {
        const std::vector<float> in{-120.0f, -90.5f, -33.25f, -140.0f, -101.0f};
        const auto out = smooth(in, 0);
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (out[i] != in[i]) {
                std::fprintf(stderr, "FAIL: off changed point %zu\n", i);
                ++failures;
            }
        }
    }

    // A flat floor stays where it is at any width.
    {
        const std::vector<float> in(64, -117.0f);
        const auto out = smooth(in, 5);
        for (std::size_t i = 0; i < out.size(); ++i) {
            expectNear("flat floor", out[i], -117.0f);
        }
    }

    // Averaging is in linear power, not dB: two neighbors at -100 dB and one
    // at -80 dB average to 10*log10((1e-10 + 1e-10 + 1e-8) / 3), about
    // -84.6 dB. A dB mean would read -93.3.
    {
        const std::vector<float> in{-100.0f, -80.0f, -100.0f};
        const auto out = smooth(in, 1);
        const float expected =
            static_cast<float>(10.0 * std::log10((1e-10 + 1e-8 + 1e-10) / 3.0));
        expectNear("linear-power mean", out[1], expected);
    }

    // An impulse spreads across exactly 2*halfWidth+1 points.
    {
        std::vector<float> in(41, -150.0f);
        in[20] = -50.0f;
        const auto out = smooth(in, 3);
        for (int i = 0; i < 41; ++i) {
            const bool inside = i >= 17 && i <= 23;
            if (inside != (out[static_cast<std::size_t>(i)] > -140.0f)) {
                std::fprintf(stderr, "FAIL: impulse spread wrong at %d (%.2f)\n",
                             i, out[static_cast<std::size_t>(i)]);
                ++failures;
            }
        }
        expectNear("impulse level", out[20],
                   static_cast<float>(10.0 * std::log10((1e-5 + 6e-15) / 7.0)));
    }

    // Edges shrink the window instead of padding it: point 0 with halfWidth 2
    // averages points 0..2 only.
    {
        const std::vector<float> in{-60.0f, -70.0f, -80.0f, -10.0f, -10.0f};
        const auto out = smooth(in, 2);
        expectNear("left edge", out[0],
                   static_cast<float>(10.0 * std::log10((1e-6 + 1e-7 + 1e-8) / 3.0)));
    }

    // A noise point next to a +10 dBm carrier stays exact. A running prefix
    // sum loses this to cancellation once the window has left the carrier.
    {
        std::vector<float> in(32, -140.0f);
        in[0] = 10.0f;
        const auto out = smooth(in, 2);
        expectNear("floor after carrier", out[20], -140.0f);
    }

    // In place is allowed.
    {
        std::vector<float> buf{-100.0f, -80.0f, -100.0f};
        std::vector<double> scratch;
        AetherSDR::smoothFftTraceLinearPower(buf, buf, 1, scratch);
        expectNear("in place", buf[1],
                   static_cast<float>(10.0 * std::log10((1e-10 + 1e-8 + 1e-10) / 3.0)));
    }

    return failures == 0 ? 0 : 1;
}
