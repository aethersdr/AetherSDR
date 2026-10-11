#include "core/dsp/SpectralEnvelope.h"
#include "gui/FftTraceDecomb.h"
#include "gui/NoiseFloorEstimator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

// FFT De-comb on the voice comb measured off a 40 m panadapter: 15 Hz per
// point, a 2.7 kHz SSB signal whose pitch harmonics sit 105 Hz (one talker)
// or 180 Hz (another) apart, 18 dB peak to valley, De-comb at 300 Hz.

namespace {

int failures = 0;

void check(bool ok, const char* what, double got)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (got %.2f)\n", what, got);
        ++failures;
    }
}

constexpr double kHzPerPoint = 15.0;
constexpr int kPoints = 600;
constexpr int kSigLo = 200;               // 2.7 kHz = 180 points
constexpr int kSigHi = 380;
constexpr float kFloor = -130.0f;
constexpr float kPeak = -80.0f;
constexpr float kCombDepth = 18.0f;
constexpr double kDecombHz = 300.0;

// The signal's outline: flat top with a gentle tilt, steep filter skirts.
float outline(int i)
{
    if (i < kSigLo || i > kSigHi) {
        return kFloor;
    }
    return kPeak - 4.0f * static_cast<float>(i - kSigLo) / static_cast<float>(kSigHi - kSigLo);
}

std::vector<float> voiceComb(double spacingHz)
{
    const double s = spacingHz / kHzPerPoint;
    std::vector<float> v(kPoints);
    for (int i = 0; i < kPoints; ++i) {
        const float e = outline(i);
        if (e == kFloor) {
            v[static_cast<std::size_t>(i)] = kFloor;
            continue;
        }
        const double ph = std::sin(std::numbers::pi * (i - kSigLo) / s);
        v[static_cast<std::size_t>(i)] = e - kCombDepth * static_cast<float>(ph * ph);
    }
    return v;
}

// Unaveraged noise floor: exponential power, deterministic LCG.
std::vector<float> noiseFloor(int n, float floorDb, std::uint32_t seed)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        const double u = (static_cast<double>(seed >> 8) + 0.5) / 16777216.0;
        x = floorDb + static_cast<float>(10.0 * std::log10(-std::log(u)));
    }
    return v;
}

std::vector<float> decomb(const std::vector<float>& in, AetherSDR::SpectralEnvelope& env)
{
    std::vector<float> out(in.size());
    const bool done = env.compute(in, out, kDecombHz / kHzPerPoint);
    check(done, "envelope plan available on an uncontended lock", 0);
    AetherSDR::decombFloorGate(in, out, AetherSDR::estimateNoiseFloorDbm(in));
    return out;
}

float median(std::vector<float> v)
{
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

} // namespace

int main()
{
    AetherSDR::SpectralEnvelope env;
    {
        for (const double spacing : {105.0, 180.0}) {
            std::vector<float> in = voiceComb(spacing);
            // Real noise under and around the signal, so the gate is exercised.
            const auto noise = noiseFloor(kPoints, kFloor, 12345u);
            for (int i = 0; i < kPoints; ++i) {
                auto& x = in[static_cast<std::size_t>(i)];
                x = 10.0f * std::log10(std::pow(10.0f, x / 10.0f)
                                       + std::pow(10.0f, noise[static_cast<std::size_t>(i)] / 10.0f));
            }
            const auto out = decomb(in, env);

            // Inside the signal, one window in from each skirt, the comb is
            // gone: every point within ~2 dB of the outline, valleys included.
            const int margin = static_cast<int>(kDecombHz / kHzPerPoint);
            double worstBelow = 0.0, worstAbove = 0.0;
            for (int i = kSigLo + margin; i <= kSigHi - margin; ++i) {
                const double d = out[static_cast<std::size_t>(i)] - outline(i);
                worstBelow = std::min(worstBelow, d);
                worstAbove = std::max(worstAbove, d);
            }
            std::fprintf(stderr, "%3.0f Hz: in-signal %+.2f..%+.2f dB of outline\n",
                         spacing, worstBelow, worstAbove);
            // Band-limited, so it rounds a little either side of the peaks.
            check(worstBelow > -2.0, "comb filled to the outline", worstBelow);
            check(worstAbove < 3.0, "no overshoot above the outline", worstAbove);

            // The outline keeps its width: two windows outside the skirt the
            // trace is back on the floor.
            const float outside = std::max(out[static_cast<std::size_t>(kSigLo - 2 * margin)],
                                           out[static_cast<std::size_t>(kSigHi + 2 * margin)]);
            check(outside < kFloor + 12.0f, "skirts not widened", outside);

            // A smooth curve rounds the skirt; pin how far.
            // Its half-way point between floor and peak must stay within one
            // window of the true edge on both sides.
            const float half = 0.5f * (kFloor + kPeak);
            int riseAt = 0;
            while (riseAt < kPoints && out[static_cast<std::size_t>(riseAt)] < half) {
                ++riseAt;
            }
            int fallAt = kPoints - 1;
            while (fallAt > 0 && out[static_cast<std::size_t>(fallAt)] < half) {
                --fallAt;
            }
            check(std::abs(riseAt - kSigLo) <= margin, "rising skirt within one window",
                  riseAt - kSigLo);
            check(std::abs(fallAt - kSigHi) <= margin, "falling skirt within one window",
                  fallAt - kSigHi);

            // The noise floor away from the signal does not rise.
            std::vector<float> floorIn(in.begin(), in.begin() + 120);
            std::vector<float> floorOut(out.begin(), out.begin() + 120);
            const double rise = median(floorOut) - median(floorIn);
            std::fprintf(stderr, "%3.0f Hz: floor median rise %+.2f dB\n", spacing, rise);
            check(rise < 1.0, "noise floor kept", rise);
        }
    }

    // Too few points per window (a wide span) is a no-op.
    {
        std::vector<float> v(64, -100.0f), out(64, 0.0f);
        check(!env.compute(v, out, 1.5), "sub-2-point window declined", 0);
    }

    return failures == 0 ? 0 : 1;
}
