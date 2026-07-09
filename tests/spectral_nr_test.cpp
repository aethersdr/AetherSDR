// Standalone test harness for SpectralNR Bessel function fix.
// CMake target `spectral_nr_test`.  Exit 0 = pass.
//
// Regression for #1507: bessI0e / bessI1e must be finite at all v up to
// GammaMax (1e4), and computeGainGamma must not NaN-clamp to 0.01 for
// strong signals under the Gamma gain method.

#include "core/SpectralNR.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>
#include <vector>

using AetherSDR::SpectralNR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const char* detail = nullptr)
{
    std::printf("%s %-70s%s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail ? detail : "");
    if (!ok) ++g_failed;
}

// ─── Reference unscaled Bessel I0 / I1 (A&S, safe only for small x) ──────────
// Used only for equivalence checks at x values that don't overflow.

double bessI0_ref(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
    }
    double t = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double bessI1_ref(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    double t = 3.75 / ax;
    double val = (std::exp(ax) / std::sqrt(ax))
               * (0.39894228 + t * (-0.03988024 + t * (-0.00362018
                + t * (0.00163801 + t * (-0.01031555 + t * (0.02282967
                + t * (-0.02895312 + t * (0.01787654 - t * 0.00420059))))))));
    return x < 0.0 ? -val : val;
}

// ─── Local copies of the scaled Bessel functions ─────────────────────────────
// bessI0e / bessI1e are private statics in SpectralNR.  We keep the same
// A&S polynomial coefficients here so we can test the math independently,
// then rely on SpectralNR::process() for full integration coverage.

double bessI0e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return std::exp(-ax) * (1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    double t = 3.75 / ax;
    return (1.0 / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double bessI1e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = std::exp(-ax) * ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    double t = 3.75 / ax;
    double val = (1.0 / std::sqrt(ax))
               * (0.39894228 + t * (-0.03988024 + t * (-0.00362018
                + t * (0.00163801 + t * (-0.01031555 + t * (0.02282967
                + t * (-0.02895312 + t * (0.01787654 - t * 0.00420059))))))));
    return x < 0.0 ? -val : val;
}

// ─── Test groups ─────────────────────────────────────────────────────────────

void test_bessel_finiteness()
{
    // For each v in this set, bessI0e(v/2) and bessI1e(v/2) must be finite.
    // v up to GammaMax = 1e4.  Values above 1420 triggered the old overflow.
    const std::vector<double> v_values = {
        0.0, 0.1, 1.0, 10.0, 100.0, 500.0,
        1000.0, 1419.0, 1420.0, 1421.0,   // straddle old overflow threshold
        2000.0, 5000.0, 10000.0
    };

    for (double v : v_values) {
        double arg = 0.5 * v;
        double i0 = bessI0e(arg);
        double i1 = bessI1e(arg);

        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e(%.1f) finite [v=%.0f]", arg, v);
        report(name, std::isfinite(i0));

        std::snprintf(name, sizeof(name), "bessI1e(%.1f) finite [v=%.0f]", arg, v);
        report(name, std::isfinite(i1));

        // Scaled Bessel must be non-negative (I0 is always >= 1, I1 >= 0 for x >= 0)
        std::snprintf(name, sizeof(name), "bessI0e(%.1f) >= 0 [v=%.0f]", arg, v);
        report(name, i0 >= 0.0);

        std::snprintf(name, sizeof(name), "bessI1e(%.1f) >= 0 for x>=0 [v=%.0f]", arg, v);
        report(name, arg == 0.0 || i1 >= 0.0);
    }
}

void test_bessel_equivalence()
{
    // At x values where the old unscaled code didn't overflow (<= 50),
    // bessI0e(x) must equal exp(-x) * bessI0_ref(x) to within 1e-9 relative.
    const std::vector<double> xs = {0.01, 0.1, 0.5, 1.0, 2.0, 3.0, 3.74, 3.75, 4.0, 5.0, 10.0, 30.0, 50.0};
    constexpr double kTol = 1e-9;

    for (double x : xs) {
        double scale = std::exp(-x);
        double ref0 = scale * bessI0_ref(x);
        double ref1 = scale * bessI1_ref(x);

        double got0 = bessI0e(x);
        double got1 = bessI1e(x);

        double err0 = std::abs(got0 - ref0) / std::max(std::abs(ref0), 1e-300);
        double err1 = std::abs(got1 - ref1) / std::max(std::abs(ref1), 1e-300);

        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e(%.4f) matches exp(-x)*I0_ref (rel err=%.2e)", x, err0);
        report(name, err0 < kTol);

        std::snprintf(name, sizeof(name), "bessI1e(%.4f) matches exp(-x)*I1_ref (rel err=%.2e)", x, err1);
        report(name, err1 < kTol);
    }

    // bessI0e is symmetric (I0 is even): bessI0e(-x) == bessI0e(x)
    for (double x : {1.0, 3.75, 10.0, 50.0}) {
        double pos = bessI0e(x);
        double neg = bessI0e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e symmetry at x=%.1f", x);
        report(name, std::abs(pos - neg) < 1e-15);
    }

    // bessI1e is antisymmetric: bessI1e(-x) == -bessI1e(x)
    for (double x : {1.0, 3.75, 10.0, 50.0}) {
        double pos = bessI1e(x);
        double neg = bessI1e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI1e antisymmetry at x=%.1f", x);
        report(name, std::abs(pos + neg) < 1e-15);
    }
}

void test_gain_finiteness()
{
    // Feed 1000 hops of full-scale 1 kHz sine through SpectralNR (Gamma method).
    // Every output sample must be finite — no NaN from Bessel overflow.
    // SpectralNR does not clamp output to ±1.0 (that is AudioEngine's job),
    // so only finiteness is asserted here.

    SpectralNR nr;
    nr.setGainMethod(2);   // Gamma (MMSE-LSA) — the path under test

    const int hopSize = 128;
    const int sampleRate = 24000;
    const double freq = 1000.0;

    std::vector<float> inBuf(hopSize), outBuf(hopSize);
    int nanCount = 0;

    for (int hop = 0; hop < 1000; ++hop) {
        int offset = hop * hopSize;
        for (int i = 0; i < hopSize; ++i) {
            double t = static_cast<double>(offset + i) / sampleRate;
            inBuf[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * t));
        }
        nr.process(inBuf.data(), outBuf.data(), hopSize);
        for (int i = 0; i < hopSize; ++i) {
            if (!std::isfinite(outBuf[i])) ++nanCount;
        }
    }

    char detail[64];
    std::snprintf(detail, sizeof(detail), " (NaN count: %d)", nanCount);
    report("gain_finiteness: no NaN/Inf in 1000 hops of 1 kHz full-scale sine", nanCount == 0, detail);
}

std::vector<float> buildSyntheticAudio(int samples)
{
    constexpr int sampleRate = 24000;
    std::vector<float> audio(samples);
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        audio[i] = static_cast<float>(
            0.30 * std::sin(2.0 * std::numbers::pi * 720.0 * t)
          + 0.18 * std::sin(2.0 * std::numbers::pi * 1740.0 * t)
          + 0.04 * std::sin(2.0 * std::numbers::pi * 43.0 * t));
    }
    return audio;
}

std::vector<float> processWithBlockSize(const std::vector<float>& input,
                                        int blockSamples)
{
    SpectralNR nr;
    nr.setGainMethod(2);

    std::vector<float> output(input.size());
    int offset = 0;
    while (offset < static_cast<int>(input.size())) {
        const int count = std::min(blockSamples,
                                   static_cast<int>(input.size()) - offset);
        nr.process(input.data() + offset, output.data() + offset, count);
        offset += count;
    }
    return output;
}

std::vector<float> processStereoSharedMaskWithBlockSize(
    const std::vector<float>& input,
    int blockFrames)
{
    SpectralNR nr;
    nr.setGainMethod(2);

    std::vector<float> output(input.size());
    int offsetFrames = 0;
    const int totalFrames = static_cast<int>(input.size() / 2);
    while (offsetFrames < totalFrames) {
        const int count = std::min(blockFrames, totalFrames - offsetFrames);
        nr.processStereoSharedMask(input.data() + (2 * offsetFrames),
                                   output.data() + (2 * offsetFrames),
                                   count);
        offsetFrames += count;
    }
    return output;
}

std::pair<double, double> stereoRmsAfter(const std::vector<float>& interleaved,
                                         int startFrame)
{
    double leftSum = 0.0;
    double rightSum = 0.0;
    int count = 0;
    const int totalFrames = static_cast<int>(interleaved.size() / 2);
    for (int frame = startFrame; frame < totalFrames; ++frame) {
        const double left = interleaved[2 * frame];
        const double right = interleaved[2 * frame + 1];
        leftSum += left * left;
        rightSum += right * right;
        ++count;
    }
    return {
        std::sqrt(leftSum / std::max(count, 1)),
        std::sqrt(rightSum / std::max(count, 1)),
    };
}

void test_block_size_invariance()
{
    // KiwiSDR audio arrives in packet-sized bursts, while native Flex RX audio
    // typically reaches NR2 at the 128-sample hop cadence. NR2 must produce the
    // same stream either way; otherwise burst-sized calls can wrap the OLA ring
    // and sound like rapid periodic audio gaps.
    const std::vector<float> input = buildSyntheticAudio(24000);
    const std::vector<float> hopOutput = processWithBlockSize(input, 128);
    const std::vector<float> packetOutput = processWithBlockSize(input, 1024);

    double maxAbsDiff = 0.0;
    for (std::size_t i = 0; i < hopOutput.size(); ++i) {
        maxAbsDiff = std::max(maxAbsDiff,
                              static_cast<double>(std::abs(hopOutput[i] - packetOutput[i])));
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), " (max abs diff: %.3e)", maxAbsDiff);
    report("block_size_invariance: 1024-sample packets match 128-sample hops",
           maxAbsDiff < 1e-7, detail);
}

void test_stereo_shared_mask_preserves_balance()
{
    // Flex remote_audio_rx is one radio-mixed stereo stream.  NR2 should use a
    // shared noise estimate, but must not collapse the stream to mono and then
    // rebuild it with one active-slice pan value (#4035).
    constexpr int sampleRate = 24000;
    constexpr int frames = sampleRate;
    std::vector<float> input(frames * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const float signal = static_cast<float>(
            0.34 * std::sin(2.0 * std::numbers::pi * 720.0 * t)
          + 0.13 * std::sin(2.0 * std::numbers::pi * 1740.0 * t)
          + 0.03 * std::sin(2.0 * std::numbers::pi * 43.0 * t));
        input[2 * i] = 0.80f * signal;
        input[2 * i + 1] = 0.20f * signal;
    }

    const std::vector<float> output =
        processStereoSharedMaskWithBlockSize(input, 1024);

    constexpr int discardFrames = 4096;
    const auto [inLeft, inRight] = stereoRmsAfter(input, discardFrames);
    const auto [outLeft, outRight] = stereoRmsAfter(output, discardFrames);
    const double inRatio = inLeft / std::max(inRight, 1e-12);
    const double outRatio = outLeft / std::max(outRight, 1e-12);
    const double ratioError = std::abs(outRatio - inRatio);

    char detail[128];
    std::snprintf(detail, sizeof(detail),
                  " (input L/R %.4f, output L/R %.4f, err %.3e)",
                  inRatio, outRatio, ratioError);
    report("stereo_shared_mask: preserves unbalanced L/R energy",
           ratioError < 1e-3, detail);

    std::snprintf(detail, sizeof(detail),
                  " (output L %.6f, R %.6f)", outLeft, outRight);
    report("stereo_shared_mask: output remains non-silent",
           outLeft > 1e-5 && outRight > 1e-5, detail);
}

void test_gain_formula_extreme_v()
{
    // Directly verify the Ephraim-Malah gain formula at v values that caused
    // NaN with the old unscaled Bessel functions (v > 1420).
    //
    // With the fix, bessI0e(v/2) / bessI1e(v/2) are bounded by 1/sqrt(v/2) * poly
    // for large v, so the gain expression is finite and approaches ~1.0 for very
    // large v (pass-through for a strong signal).
    //
    // Constants match SpectralNR internals:
    constexpr double gf1p5    = 0.8862269254527580; // sqrt(pi)/2 = Gamma(3/2)
    constexpr double GammaMax = 1e4;
    constexpr double EpsFloor = 1e-300;

    // Test at v values straddling and far above the old overflow threshold.
    const std::vector<double> v_values = {1419.0, 1420.0, 1421.0, 2000.0, 5000.0, 10000.0};

    for (double v : v_values) {
        // Use gamma = GammaMax — the worst case (maximum a-posteriori SNR cap).
        double gamma = GammaMax;

        // Compute the gain formula directly using the local bessI0e/bessI1e.
        double gain = gf1p5 * std::sqrt(v) / std::max(gamma, EpsFloor)
                    * ((1.0 + v) * bessI0e(0.5 * v) + v * bessI1e(0.5 * v));

        char name[128];
        std::snprintf(name, sizeof(name),
                      "gain_formula finite at v=%.0f", v);
        report(name, std::isfinite(gain));

        std::snprintf(name, sizeof(name),
                      "gain_formula >= 0 at v=%.0f", v);
        report(name, gain >= 0.0);

        std::snprintf(name, sizeof(name),
                      "gain_formula not NaN-clamp sentinel (0.01) at v=%.0f", v);
        report(name, gain > 0.05);   // 0.01 sentinel is well below this

        // For large v, the pre-clamp formula approaches v/gamma ≈ 1.0; small
        // floating-point overshoot is expected and clamped by computeGainGamma.
        std::snprintf(name, sizeof(name),
                      "gain_formula <= 1.1 (pre-clamp, v=%.0f, gain=%.6f)", v, gain);
        report(name, gain <= 1.1);
    }
}

} // namespace

int main()
{
    std::printf("\n=== spectral_nr_test ===\n\n");

    std::printf("-- Bessel finiteness (old threshold: v=1420) --\n");
    test_bessel_finiteness();

    std::printf("\n-- Bessel mathematical equivalence (x <= 50) --\n");
    test_bessel_equivalence();

    std::printf("\n-- Gain finiteness (1000 hops, full-scale 1 kHz sine) --\n");
    test_gain_finiteness();

    std::printf("\n-- Block-size invariance (Kiwi packet cadence regression) --\n");
    test_block_size_invariance();

    std::printf("\n-- Stereo shared-mask balance preservation (#4035) --\n");
    test_stereo_shared_mask_preserves_balance();

    std::printf("\n-- Gain formula at extreme v (NaN-clamp regression) --\n");
    test_gain_formula_extreme_v();

    std::printf("\n%s — %d test(s) failed\n\n",
                g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
