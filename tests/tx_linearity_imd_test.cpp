// TX Linearity Analyzer — IMD measurement tests against a synthetic PA.
//
// Every assertion here compares the analyzer's answer against a CLOSED-FORM
// expectation derived from the polynomial's coefficients (see
// TxLinearityTestSignals.h), not against a previously recorded output. That
// distinction is the difference between a test that proves the instrument
// measures correctly and a test that proves it still does whatever it did last
// week.

#include "core/txlinearity/ITxCaptureSource.h"
#include "core/txlinearity/ImdAnalyzer.h"
#include "core/txlinearity/TxSpectrumAnalyzer.h"

#include "TxLinearityTestSignals.h"

#include <cmath>
#include <cstdio>
#include <optional>

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
constexpr int kFrames = 8;

const ImdProduct* findProduct(const LinearityResult& r, int order, bool upper)
{
    for (const ImdProduct& p : r.products) {
        if (p.order == order && p.upper == upper) {
            return &p;
        }
    }
    return nullptr;
}

LinearityResult analyze(const std::vector<std::complex<double>>& x,
                        const ImdConfig& cfg = {})
{
    TxSpectrumAnalyzer an(kFftSize, WindowKind::BlackmanHarris7);
    for (int f = 0; f < kFrames; ++f) {
        an.accumulate(toFloat(x, std::size_t(f) * std::size_t(kFftSize),
                              std::size_t(kFftSize)));
    }
    return measureTwoTone(an, kSampleRate, cfg, false);
}

// Off-bin on purpose — see the leakage discussion in the spectrum test.
double toneF1()
{
    return -1000.0 + 0.29 * TxSpectrumAnalyzer::binHz(kSampleRate, kFftSize);
}

}  // namespace

int main()
{
    const double f1 = toneF1();
    const double spacing = 2000.0;
    const double f2 = f1 + spacing;
    const std::size_t nSamples = std::size_t(kFftSize) * std::size_t(kFrames);

    // ── Known-coefficient PA, analytic IMD3/IMD5 ────────────────────────────
    {
        // Compressive third order (a3 negative, as a real PA is) plus a small
        // fifth. Chosen so IMD3 lands near -35 dBc and IMD5 near -60 dBc —
        // realistic levels, both comfortably above the synthetic noise floor,
        // and far enough apart that a bug swapping the two is visible.
        const double amp = 0.30;
        const MemoryPolynomialPa pa(1.0, -0.55, 0.12);
        const std::optional<MemoryPolynomialPa::AnalyticLevels> want = pa.analytic(amp);
        check(want.has_value(), "memoryless model has a closed form");

        std::vector<std::complex<double>> x =
            twoTone(nSamples, kSampleRate, f1, f2, amp);
        x = pa.apply(x);

        const LinearityResult r = analyze(x);
        check(r.valid, "distorted two-tone measures successfully");
        checkNear(r.toneSpacingHz, spacing, 0.5, "tone spacing recovered");
        checkNear(r.toneImbalanceDb, 0.0, 0.05, "balanced tones report no imbalance");

        const double toneDb = 20.0 * std::log10(std::abs(want->toneAmplitude));
        checkNear(r.tone1.powerDb, toneDb, 0.1, "lower tone amplitude matches the model");
        checkNear(r.tone2.powerDb, toneDb, 0.1, "upper tone amplitude matches the model");

        // The target from the spec: +/-0.5 dB for products at least 20 dB above
        // the synthetic noise floor. The analyzer does considerably better than
        // that on a noiseless synthetic, so hold it to 0.3 dB — a regression
        // that merely stays inside 0.5 dB should still be visible.
        for (bool upper : {false, true}) {
            const ImdProduct* p3 = findProduct(r, 3, upper);
            check(p3 != nullptr, "IMD3 product located");
            if (p3 != nullptr) {
                checkNear(p3->dbcPerTone, want->imd3DbcPerTone(), 0.3,
                          "IMD3 dBc/tone matches the analytic value");
                check(!p3->limitedByNoiseFloor, "IMD3 is not noise-limited here");
                checkNear(p3->freqHz, upper ? f2 + spacing : f1 - spacing, 2.0,
                          "IMD3 lands at the predicted frequency");
            }
            const ImdProduct* p5 = findProduct(r, 5, upper);
            check(p5 != nullptr, "IMD5 product located");
            if (p5 != nullptr) {
                checkNear(p5->dbcPerTone, want->imd5DbcPerTone(), 0.3,
                          "IMD5 dBc/tone matches the analytic value");
            }
        }

        // ── Reference convention ────────────────────────────────────────────
        //
        // The trap this feature must not fall into. For a balanced pair the
        // PEP-referenced and tone-referenced numbers differ by exactly
        // 10*log10(4) = 6.0206 dB, and both are reported, always labelled.
        checkNear(r.pepDb - r.tone1.powerDb, kPepOverToneDb, 0.02,
                  "measured PEP sits 6.02 dB above one tone");
        const ImdProduct* p3 = findProduct(r, 3, false);
        if (p3 != nullptr) {
            checkNear(p3->dbcPerTone - p3->dbcPep, kPepOverToneDb, 0.02,
                      "per-tone and PEP dBc differ by exactly 6.02 dB");
        }

        // Shoulder and ACPR are engaged and sit below the tones. Their absolute
        // level depends on the whole skirt, so assert the ordering and the
        // reference consistency rather than a value the model does not predict
        // in closed form.
        check(r.shoulderLow.has_value() && r.shoulderHigh.has_value(),
              "shoulder levels measured");
        check(r.acprLow.has_value() && r.acprHigh.has_value(), "ACPR measured");
        if (r.acprLow.has_value()) {
            check(r.acprLow->dbcPerTone < 0.0, "lower ACPR sits below the tone level");
            checkNear(r.acprLow->dbcPerTone - r.acprLow->dbcPep, kPepOverToneDb, 0.02,
                      "ACPR carries both reference conventions consistently");
        }
    }

    // ── Imbalanced tones ────────────────────────────────────────────────────
    //
    // With unequal tones the exact 6.02 dB PEP relation stops holding, and the
    // analyzer must report the measured envelope rather than the idealisation —
    // the idealisation is wrong in the direction that flatters the radio.
    {
        const double a1 = 0.30;
        const double a2 = a1 * std::pow(10.0, -1.5 / 20.0);   // 1.5 dB down
        std::vector<std::complex<double>> x =
            twoTone(nSamples, kSampleRate, f1, f2, a1, a2);
        x = MemoryPolynomialPa(1.0, -0.55, 0.0).apply(x);

        const LinearityResult r = analyze(x);
        check(r.valid, "imbalanced two-tone measures successfully");

        // The OUTPUT imbalance is not the input imbalance. Expanding
        // y = a1 x + a3|x|^2 x for x = A1 e1 + A2 e2 gives fundamental
        // coefficients
        //     e1: a1 A1 + a3 (A1^3 + 2 A1 A2^2)
        //     e2: a1 A2 + a3 (A2^3 + 2 A1^2 A2)
        // — each tone is compressed by its own amplitude AND cross-modulated by
        // the other's, and the two effects do not cancel. A compressive a3
        // therefore WIDENS a 1.5 dB input imbalance to about 1.64 dB at the
        // output. Asserting the input figure here would have been asserting
        // that the analyzer fails to see real cross-modulation.
        const double a3 = -0.55;
        const double out1 = a1 + a3 * (a1 * a1 * a1 + 2.0 * a1 * a2 * a2);
        const double out2 = a2 + a3 * (a2 * a2 * a2 + 2.0 * a1 * a1 * a2);
        const double expectedImbalance = 20.0 * std::log10(out2 / out1);
        checkNear(r.toneImbalanceDb, expectedImbalance, 0.05,
                  "tone imbalance matches the model's output imbalance");
        check(expectedImbalance < -1.5,
              "and the compressive PA has widened the input imbalance, not narrowed it");

        // Envelope peak is A1 + A2, so PEP sits strictly below the balanced
        // idealisation of one tone + 6.02 dB. Reporting the idealisation would
        // be wrong in the direction that flatters the radio.
        check(r.pepDb - r.tone1.powerDb < kPepOverToneDb,
              "an imbalanced pair reports less PEP headroom than 6.02 dB");
        checkNear(r.pepDb, 20.0 * std::log10(out1 + out2), 0.1,
                  "measured PEP matches the model's envelope peak");
    }

    // ── Noise floor honesty ─────────────────────────────────────────────────
    //
    // The instrument must say "I cannot measure this" rather than emit a
    // confident wrong number. A measurement that is actually reading the
    // analyzer's own noise is worse than no measurement at all.
    {
        // Very nearly linear: IMD3 around -85 dBc, buried under the noise added
        // below.
        const double amp = 0.30;
        std::vector<std::complex<double>> x =
            twoTone(nSamples, kSampleRate, f1, f2, amp);
        x = MemoryPolynomialPa(1.0, -1.5e-4, 0.0).apply(x);
        addNoise(x, 3e-4, 0x5EEDu);

        const LinearityResult r = analyze(x);
        check(r.valid, "a noisy capture still measures the fundamentals");

        // Per-quadrature sigma s gives total noise power 2s^2. The reported
        // floor is PER ANALYSIS BIN, and a bin's noise power is not simply
        // 2s^2/N — the window's equivalent noise bandwidth comes into it:
        //
        //   E|S[k]|^2 = 2 s^2 * sum(w^2)/sum(w)^2 = 2 s^2 * ENBW / N
        //
        // For the 7-term Blackman-Harris, ENBW is about 2.63 bins, so the floor
        // sits about 4.2 dB above the naive 2s^2/N figure. Getting this wrong in
        // the other direction — reporting the naive number — would make every
        // product look 4 dB more trustworthy than it is. Welch averaging reduces
        // the VARIANCE of this estimate, not its mean.
        const double enbw = makeWindow(WindowKind::BlackmanHarris7,
                                       std::size_t(kFftSize)).enbwBins;
        const double expectedFloorDb =
            10.0 * std::log10(2.0 * 3e-4 * 3e-4 * enbw / double(kFftSize));
        checkNear(r.noiseFloorDb, expectedFloorDb, 1.0,
                  "reported noise floor matches the injected noise through the window ENBW");

        const ImdProduct* p3 = findProduct(r, 3, false);
        check(p3 != nullptr, "a product is still located in noise");
        if (p3 != nullptr) {
            check(p3->limitedByNoiseFloor,
                  "a product buried in noise is flagged rather than quoted");
        }
    }

    // ── Refusals ────────────────────────────────────────────────────────────
    {
        // Nothing accumulated at all.
        TxSpectrumAnalyzer empty(kFftSize, WindowKind::BlackmanHarris7);
        const LinearityResult r = measureTwoTone(empty, kSampleRate, ImdConfig{}, false);
        check(!r.valid, "an empty analyzer refuses to measure");
        check(!r.invalidReason.empty(), "the refusal carries a reason");

        // Two tones closer together than the window can separate must be
        // refused, not reported as one tone twice with a nonsense spacing.
        const double tooClose = 3.0 * TxSpectrumAnalyzer::binHz(kSampleRate, kFftSize);
        const std::vector<std::complex<double>> x =
            twoTone(nSamples, kSampleRate, f1, f1 + tooClose, 0.3);
        const LinearityResult rc = analyze(x);
        check(!rc.valid, "unresolvable tone spacing is refused");
    }

    // ── Clip detection is a population, not a sample ─────────────────────────
    {
        std::vector<std::complex<float>> frame(4096, std::complex<float>(0.1f, 0.0f));
        frame[7] = std::complex<float>(1.0f, 0.0f);
        check(saturatedFraction(frame) < kClippedFractionThreshold,
              "one full-scale sample in 4096 is not clipping");
        for (std::size_t i = 0; i < 512; ++i) {
            frame[i] = std::complex<float>(1.0f, 0.0f);
        }
        check(saturatedFraction(frame) > kClippedFractionThreshold,
              "a saturated population is clipping");
    }

    if (g_failures == 0) {
        std::printf("tx_linearity_imd_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
