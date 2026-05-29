// Standalone test harness for SpectralNR Bessel function fix.
// CMake target `spectral_nr_test`.  Exit 0 = pass.
//
// Regression for #1507: bessI0e / bessI1e must be finite at all v up to
// GammaMax (1e4), and computeGainGamma must not NaN-clamp to 0.01 for
// strong signals under the Gamma gain method.

#include "core/SpectralNR.h"

#include <cmath>
#include <cstdio>
#include <numbers>
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

// ─── Expose internal Bessel functions via subclass ────────────────────────────
// SpectralNR declares bessI0e / bessI1e as private static — a thin subclass
// exposes them for direct unit testing without modifying production headers.

class SpectralNRTestable : public SpectralNR {
public:
    SpectralNRTestable() : SpectralNR(256, 24000) {}
    static double i0e(double x) { return bessI0e(x); }
    static double i1e(double x) { return bessI1e(x); }
};

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
        double i0 = SpectralNRTestable::i0e(arg);
        double i1 = SpectralNRTestable::i1e(arg);

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

        double got0 = SpectralNRTestable::i0e(x);
        double got1 = SpectralNRTestable::i1e(x);

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
        double pos = SpectralNRTestable::i0e(x);
        double neg = SpectralNRTestable::i0e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI0e symmetry at x=%.1f", x);
        report(name, std::abs(pos - neg) < 1e-15);
    }

    // bessI1e is antisymmetric: bessI1e(-x) == -bessI1e(x)
    for (double x : {1.0, 3.75, 10.0, 50.0}) {
        double pos = SpectralNRTestable::i1e(x);
        double neg = SpectralNRTestable::i1e(-x);
        char name[128];
        std::snprintf(name, sizeof(name), "bessI1e antisymmetry at x=%.1f", x);
        report(name, std::abs(pos + neg) < 1e-15);
    }
}

void test_gain_finiteness()
{
    // Feed 1000 hops of full-scale 1 kHz sine through SpectralNR with default
    // settings (Gamma method, gainMax=1.0).  Every output sample must be finite
    // and within [-1.0, 1.0].  No NaN-clamp suppression burst = no crackling.

    SpectralNR nr(256, 24000);
    nr.setGainMethod(2);   // Gamma (MMSE-LSA) — the path under test

    const int hopSize = 128;
    const int sampleRate = 24000;
    const double freq = 1000.0;
    const int totalSamples = hopSize * 1000;

    std::vector<float> inBuf(hopSize), outBuf(hopSize);
    int nanCount = 0;
    int clipCount = 0;

    for (int hop = 0; hop < 1000; ++hop) {
        int offset = hop * hopSize;
        for (int i = 0; i < hopSize; ++i) {
            double t = static_cast<double>(offset + i) / sampleRate;
            inBuf[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * t));
        }
        nr.process(inBuf.data(), outBuf.data(), hopSize);
        for (int i = 0; i < hopSize; ++i) {
            if (!std::isfinite(outBuf[i])) ++nanCount;
            if (std::abs(outBuf[i]) > 1.0f + 1e-6f) ++clipCount;
        }
    }

    char detail[64];
    std::snprintf(detail, sizeof(detail), " (NaN count: %d)", nanCount);
    report("gain_finiteness: no NaN in 1000 hops of 1 kHz full-scale sine", nanCount == 0, detail);

    std::snprintf(detail, sizeof(detail), " (clip count: %d)", clipCount);
    report("gain_finiteness: output within [-1,1] throughout", clipCount == 0, detail);
}

void test_gain_high_snr()
{
    // Directly verify that computeGainGamma produces a finite, valid gain in
    // [0, gainMax] for the high-v regime that previously triggered NaN.
    // We do this by feeding a sustained strong signal and checking that the
    // SpectralNR output is never near-silent (which the 0.01 NaN-clamp would
    // cause) after the filter has converged (skip first 200 hops for ramp-up).

    SpectralNR nr(256, 24000);
    nr.setGainMethod(2);

    const int hopSize = 128;
    const int sampleRate = 24000;
    const double freq = 800.0;

    std::vector<float> inBuf(hopSize), outBuf(hopSize);

    // Warm up (ramp period ~187 hops, defined in SpectralNR as RampFrames)
    for (int hop = 0; hop < 250; ++hop) {
        for (int i = 0; i < hopSize; ++i) {
            double t = static_cast<double>(hop * hopSize + i) / sampleRate;
            inBuf[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * t));
        }
        nr.process(inBuf.data(), outBuf.data(), hopSize);
    }

    // Now check: with a full-scale sine, gain must not be near-zero (0.01 clamp)
    int suppressedHops = 0;
    for (int hop = 0; hop < 200; ++hop) {
        for (int i = 0; i < hopSize; ++i) {
            double t = static_cast<double>((250 + hop) * hopSize + i) / sampleRate;
            inBuf[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * t));
        }
        nr.process(inBuf.data(), outBuf.data(), hopSize);

        // Compute output RMS for this hop
        double sumSq = 0.0;
        for (int i = 0; i < hopSize; ++i)
            sumSq += static_cast<double>(outBuf[i]) * outBuf[i];
        double rms = std::sqrt(sumSq / hopSize);

        // If gain had NaN-clamped to 0.01, output RMS would be ~0.007.
        // A correctly processed strong sine should have RMS well above 0.1.
        if (rms < 0.05) ++suppressedHops;
    }

    char detail[64];
    std::snprintf(detail, sizeof(detail), " (suppressed hops: %d / 200)", suppressedHops);
    report("gain_high_snr: no near-silent suppression hops after convergence", suppressedHops == 0, detail);
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

    std::printf("\n-- Gain stability at high SNR (no 0.01 NaN-clamp bursts) --\n");
    test_gain_high_snr();

    std::printf("\n%s — %d test(s) failed\n\n",
                g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
