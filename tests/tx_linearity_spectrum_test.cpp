// TX Linearity Analyzer — window and spectrum-estimator tests.
//
// The headline case here is the WINDOW LEAKAGE REGRESSION: synthesize a
// two-tone with NO distortion whatsoever and assert the analyzer reports IMD
// products down at the window's own sidelobe floor. This catches the single
// most likely class of bug in the whole feature — an analyzer that measures its
// own window skirt and calls it the amplifier's IMD3 looks completely
// plausible, produces confident numbers, and is wrong about every radio it is
// ever pointed at.
//
// The same test is run through a Hann window as a control. Hann is expected to
// FAIL to see a clean signal as clean; asserting that keeps the reason for the
// Blackman-Harris default in executable form, so a future "optimization" back
// to Hann trips a test instead of quietly capping the instrument's dynamic
// range at about -50 dBc.

#include "core/txlinearity/ImdAnalyzer.h"
#include "core/txlinearity/SpectrumWindow.h"
#include "core/txlinearity/TxSpectrumAnalyzer.h"

#include "TxLinearityTestSignals.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR::txlin;
using namespace txlin_test;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void checkNear(double got, double want, double tol, const char* what)
{
    if (!(std::abs(got - want) <= tol)) {
        std::fprintf(stderr, "FAIL: %s (got %.4f, want %.4f +/- %.4f)\n",
                     what, got, want, tol);
        ++g_failures;
    }
}

namespace {

constexpr double kSampleRate = 192000.0;
constexpr int kFftSize = 16384;

// Feed `frames` consecutive blocks of a signal into an analyzer.
void feed(TxSpectrumAnalyzer& an, const std::vector<std::complex<double>>& x, int frames)
{
    for (int f = 0; f < frames; ++f) {
        const std::vector<std::complex<float>> block =
            toFloat(x, std::size_t(f) * std::size_t(kFftSize), std::size_t(kFftSize));
        check(an.accumulate(block), "accumulate accepted a full frame");
    }
}

}  // namespace

int main()
{
    // ── Window factors ──────────────────────────────────────────────────────
    //
    // Published values for the periodic form. Hann's ENBW is exactly 1.5 bins
    // and its coherent gain exactly 0.5, which makes it the arithmetic anchor:
    // if those two are right the general cosine-sum machinery is right.
    {
        const AnalysisWindow hann = makeWindow(WindowKind::Hann, 4096);
        checkNear(hann.coherentGain, 0.5, 1e-9, "Hann coherent gain is 0.5");
        checkNear(hann.enbwBins, 1.5, 1e-6, "Hann ENBW is 1.5 bins");

        const AnalysisWindow bh4 = makeWindow(WindowKind::BlackmanHarris4, 4096);
        checkNear(bh4.coherentGain, 0.35875, 1e-6, "BH4 coherent gain is a0");
        checkNear(bh4.enbwBins, 2.0044, 2e-3, "BH4 ENBW is ~2.0044 bins");

        const AnalysisWindow bh7 = makeWindow(WindowKind::BlackmanHarris7, 4096);
        checkNear(bh7.coherentGain, 0.27105140069342, 1e-6, "BH7 coherent gain is a0");
        // The 7-term window trades main-lobe width for sidelobe depth, so its
        // ENBW is correspondingly larger than the 4-term's.
        check(bh7.enbwBins > 2.5 && bh7.enbwBins < 2.8, "BH7 ENBW is ~2.63 bins");
        check(bh7.mainLobeHalfBins == 7, "BH7 main lobe is +/-7 bins");

        // A degenerate request must not produce NaN factors that would poison
        // every division downstream.
        const AnalysisWindow tiny = makeWindow(WindowKind::BlackmanHarris7, 1);
        check(tiny.taps.empty(), "a 1-tap window request yields no taps");
        check(tiny.enbwBins == 1.0, "a degenerate window keeps unity ENBW");

        check(windowKindFromKey(windowKindKey(WindowKind::FlatTop)) == WindowKind::FlatTop,
              "window kind key round-trips");
        check(windowKindFromKey("nonsense") == WindowKind::BlackmanHarris7,
              "an unknown window key falls back to the default");
    }

    // ── Tone amplitude recovery, on-bin and off-bin ─────────────────────────
    //
    // A tone parked exactly half a bin between two bins is the worst case for
    // the peak interpolator. Without interpolation a Blackman-Harris window
    // reads such a tone more than a dB low, and that error would land directly
    // in every dBc figure the instrument publishes.
    {
        const double binHz = TxSpectrumAnalyzer::binHz(kSampleRate, kFftSize);
        const double amp = 0.25;
        const double expectedDb = 20.0 * std::log10(amp);

        for (double offsetBins : {0.0, 0.25, 0.5}) {
            TxSpectrumAnalyzer an(kFftSize, WindowKind::BlackmanHarris7);
            const double f = (1000.0 + offsetBins) * binHz;
            const std::vector<std::complex<double>> x =
                twoTone(std::size_t(kFftSize) * 4, kSampleRate, f, f + 2000.0, amp);
            feed(an, x, 4);

            const int centre = kFftSize / 2;
            const int predicted = an.freqToBin(f, kSampleRate);
            const InterpolatedPeak pk = interpolatePeak(an.powerSpectrum(),
                                                        predicted - 8, predicted + 8,
                                                        kSampleRate);
            check(pk.valid, "peak interpolation succeeded");
            checkNear(pk.powerDb, expectedDb, 0.05,
                      "interpolated tone amplitude is within 0.05 dB");
            checkNear(pk.freqHz, f, binHz * 0.05,
                      "interpolated tone frequency is within 0.05 bins");
            (void)centre;
        }
    }

    // ── Window leakage regression ───────────────────────────────────────────
    //
    // A perfectly linear "amplifier": the two-tone passes through untouched, so
    // every IMD product the analyzer reports is leakage, not distortion. Tones
    // are deliberately off-bin, which is the leakage-worst case.
    {
        const double binHz = TxSpectrumAnalyzer::binHz(kSampleRate, kFftSize);
        const double f1 = (-100.0 + 0.37) * binHz;   // ~-1.17 kHz
        const double f2 = f1 + 2000.0;               // 2 kHz spacing
        const std::vector<std::complex<double>> x =
            twoTone(std::size_t(kFftSize) * 8, kSampleRate, f1, f2, 0.35);

        TxSpectrumAnalyzer bh(kFftSize, WindowKind::BlackmanHarris7);
        feed(bh, x, 8);
        ImdConfig cfg;
        const LinearityResult r = measureTwoTone(bh, kSampleRate, cfg, false);
        check(r.valid, "a clean two-tone measures successfully");
        check(!r.products.empty(), "products were located");

        double worstDbc = -1000.0;
        for (const ImdProduct& p : r.products) {
            if (p.dbcPerTone > worstDbc) {
                worstDbc = p.dbcPerTone;
            }
            // The whole point: every product on an undistorted signal must be
            // reported as unmeasurable, not as a number.
            check(p.limitedByNoiseFloor,
                  "products of an undistorted signal are flagged noise-limited");
        }
        // -80 dBc is a deliberately conservative bar against the window's
        // nominal -92 dB: floating-point synthesis, the 0.37-bin offset and the
        // averaging all contribute a little. Anywhere near -50 means the window
        // has been changed and the instrument's dynamic range with it.
        check(worstDbc < -80.0, "clean two-tone reports no product above -80 dBc");

        // The control. Hann is expected to fail this — it is here so that the
        // reason the default is Blackman-Harris is asserted, not just written
        // in a comment.
        TxSpectrumAnalyzer hn(kFftSize, WindowKind::Hann);
        feed(hn, x, 8);
        const LinearityResult rh = measureTwoTone(hn, kSampleRate, cfg, false);
        check(rh.valid, "the Hann control measures successfully");
        double hannWorst = -1000.0;
        for (const ImdProduct& p : rh.products) {
            if (p.dbcPerTone > hannWorst) {
                hannWorst = p.dbcPerTone;
            }
        }
        check(hannWorst > worstDbc + 20.0,
              "Hann leaks at least 20 dB more than Blackman-Harris 7 "
              "(this is why the default is what it is)");
    }

    // ── Band integration ────────────────────────────────────────────────────
    //
    // sum(|S|^2)/ENBW must recover a tone's power exactly, and must recover a
    // noise band's power too. One expression, both signal classes — if this is
    // wrong then ACPR and the shoulder levels are wrong in a way no amount of
    // eyeballing the plot would reveal.
    {
        TxSpectrumAnalyzer an(kFftSize, WindowKind::BlackmanHarris7);
        const double amp = 0.3;
        const double f1 = -20000.0 + 0.31 * TxSpectrumAnalyzer::binHz(kSampleRate, kFftSize);
        const std::vector<std::complex<double>> x =
            twoTone(std::size_t(kFftSize) * 4, kSampleRate, f1, f1 + 2000.0, amp);
        feed(an, x, 4);

        // A band containing exactly one of the two tones, wide enough to hold
        // its whole main lobe.
        const std::optional<BandPower> bp =
            integrateBand(an, kSampleRate, f1 - 500.0, f1 + 500.0, 0.0, 0.0);
        check(bp.has_value(), "band integration over a tone succeeded");
        if (bp.has_value()) {
            checkNear(bp->powerDb, 20.0 * std::log10(amp), 0.05,
                      "integrated tone power matches the tone amplitude");
        }

        // A band running past the capture edge must be refused, not truncated:
        // a truncated integration under-reports and would flatter the radio.
        check(!integrateBand(an, kSampleRate, kSampleRate / 2.0 - 100.0,
                             kSampleRate / 2.0 + 100.0, 0.0, 0.0).has_value(),
              "a band crossing the capture edge is refused, not truncated");
    }

    if (g_failures == 0) {
        std::printf("tx_linearity_spectrum_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
