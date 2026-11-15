#include "core/dsp/SpectralEnvelope.h"
#include "gui/FftTraceDecomb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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

AetherSDR::DecombScratch scratch;

std::vector<float> decomb(const std::vector<float>& in, AetherSDR::SpectralEnvelope& env)
{
    std::vector<float> out = in;
    const bool done = AetherSDR::decombTrace(out, kDecombHz / kHzPerPoint, env, scratch);
    check(done, "envelope plan available on an uncontended lock", 0);
    return out;
}

// Voice comb `peakDb` at the top of its outline, mixed into seeded noise.
std::vector<float> noisyVoice(double spacingHz, float peakDb, std::uint32_t seed)
{
    std::vector<float> in = voiceComb(spacingHz);
    const auto noise = noiseFloor(kPoints, kFloor, seed);
    for (int i = 0; i < kPoints; ++i) {
        auto& x = in[static_cast<std::size_t>(i)];
        const float sig = x == kFloor ? -400.0f : x + (peakDb - kPeak);
        x = 10.0f * std::log10(std::pow(10.0f, sig / 10.0f)
                               + std::pow(10.0f, noise[static_cast<std::size_t>(i)] / 10.0f));
    }
    return in;
}

float outlineAt(int i, float peakDb)
{
    return outline(i) + (peakDb - kPeak);
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
            // Real noise under and around the signal, so the gate is exercised.
            const std::vector<float> in = noisyVoice(spacing, kPeak, 12345u);
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

    // A weaker voice, ~20 dB over the noise median, is still filled.
    for (const double spacing : {105.0, 180.0}) {
        constexpr float kWeakPeak = -112.0f;
        const auto out = decomb(noisyVoice(spacing, kWeakPeak, 777u), env);
        const int margin = static_cast<int>(kDecombHz / kHzPerPoint);
        double worstBelow = 0.0;
        for (int i = kSigLo + margin; i <= kSigHi - margin; ++i) {
            worstBelow = std::min(worstBelow,
                                  double(out[static_cast<std::size_t>(i)] - outlineAt(i, kWeakPeak)));
        }
        std::fprintf(stderr, "weak %3.0f Hz: in-signal worst %+.2f dB of outline\n", spacing,
                     worstBelow);
        check(worstBelow > -2.5, "weak voice comb filled", worstBelow);
    }

    // Over plain unaveraged noise the floor is left as drawn: across 50
    // seeds, neither its median nor its points move. (An envelope keyed on
    // its own level rode the noise peaks and lifted this floor ~5 dB.)
    {
        double worstRise = 0.0, sumRise = 0.0, worstMeanAbs = 0.0;
        for (std::uint32_t seed = 1; seed <= 50; ++seed) {
            std::vector<float> in = noiseFloor(2000, kFloor, seed * 2654435761u);
            std::vector<float> out = in;
            check(AetherSDR::decombTrace(out, kDecombHz / kHzPerPoint, env, scratch),
                  "noise frame computed", 0);
            const double rise = median(out) - median(in);
            double meanAbs = 0.0;
            for (std::size_t i = 0; i < in.size(); ++i) {
                meanAbs += std::fabs(out[i] - in[i]);
            }
            meanAbs /= static_cast<double>(in.size());
            worstRise = std::max(worstRise, rise);
            sumRise += rise;
            worstMeanAbs = std::max(worstMeanAbs, meanAbs);
        }
        std::fprintf(stderr, "noise: median rise mean %+.2f worst %+.2f, worst mean|change| %.2f dB\n",
                     sumRise / 50.0, worstRise, worstMeanAbs);
        check(sumRise / 50.0 < 0.25, "noise floor median kept (mean of 50 seeds)", sumRise / 50.0);
        check(worstRise < 0.5, "noise floor median kept (worst seed)", worstRise);
        check(worstMeanAbs < 0.5, "noise points kept", worstMeanAbs);
    }

    // No-data points stay no-data: De-comb never paints an envelope into them.
    {
        std::vector<float> in = noisyVoice(180.0, kPeak, 4242u);
        for (int i = kSigHi + 2; i < kSigHi + 30; ++i) {
            in[static_cast<std::size_t>(i)] = -std::numeric_limits<float>::infinity();
        }
        in[kSigLo + 50] = std::numeric_limits<float>::quiet_NaN();
        const auto out = decomb(in, env);
        bool kept = std::isnan(out[kSigLo + 50]);
        for (int i = kSigHi + 2; i < kSigHi + 30; ++i) {
            kept = kept && std::isinf(out[static_cast<std::size_t>(i)]);
        }
        check(kept, "non-finite points left as they are", 0);
    }

    // Whether De-comb runs follows the source bins: the width must span two.
    // 512 bins over 200 kHz is ~390 Hz per bin, so 300 Hz cannot hold a comb.
    {
        using AetherSDR::decombWindowPoints;
        check(decombWindowPoints(200e3 / 511.0, 300) == 0.0, "unresolvable width is off", 0);
        const double w = decombWindowPoints(5e3 / 511.0, 300);
        check(std::abs(w - 300.0 * 511.0 / 5e3) < 1e-9, "width in source bins", w);
        check(decombWindowPoints(100.0, 200) > 0.0, "two source bins per width runs", 0);
        check(decombWindowPoints(100.0, 199) == 0.0, "under two source bins is off", 0);
        check(decombWindowPoints(5e3 / 511.0, 0) == 0.0, "zero width is off", 0);
    }

    // A drag-resize walks through many lengths; the bounded plan cache keeps
    // computing correctly as old lengths are evicted and come back.
    {
        bool ok = true;
        for (int round = 0; round < 2; ++round) {
            for (int n = 300; n < 420; n += 7) {
                std::vector<float> flat(static_cast<std::size_t>(n), -100.0f);
                std::vector<float> out(flat.size());
                ok = ok && env.compute(flat, out, 10.0)
                     && std::fabs(out[static_cast<std::size_t>(n / 2)] + 100.0f) < 0.01f;
            }
        }
        check(ok, "plan cache survives many lengths", 0);
    }

    // Too few points per window (a wide span) is a no-op.
    {
        std::vector<float> v(64, -100.0f), out(64, 0.0f);
        check(!env.compute(v, out, 1.5), "sub-2-point window declined", 0);
    }

    return failures == 0 ? 0 : 1;
}
