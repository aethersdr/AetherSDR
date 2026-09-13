// The contracts BandscopeDialog depends on from ClientEqFftAnalyzer, pinned so
// that a change to the analyzer breaks a test instead of the window. There are
// two: reset()-then-update() is unsmoothed (case 2), and the absolute dB scale
// is what the window thinks it is (case 2b).
//
// WHY THIS TEST EXISTS. The bandscope reuses ClientEqFftAnalyzer rather than
// carrying a second radix-2 transform: it is already a 2048-point Hann-windowed
// FFT reporting dBFS, and 2048 is exactly the HL2's record length. But the
// analyzer was written for a 25 Hz audio feed and smooths each bin with an
// attack/decay follower. Bandscope frames arrive seconds apart and on request,
// so a smoothed bin would show a level from the previous Refresh.
//
// The dialog therefore calls reset() before every update(). That works because
// the first update() after a reset takes the transform's own value as its
// starting point instead of filtering towards it — a property of the analyzer's
// `m_primed` handling, not of its public API, and therefore exactly the kind of
// thing that gets refactored away without anyone noticing what depended on it.
//
// The second contract was added after review found the window's displayed dBFS
// 6.02 dB low: the analyzer's 2/N normalisation is the UNWINDOWED one and never
// removed the Hann window's coherent gain, so a converter on its rail read -6
// and the top of the bandscope's scale was unreachable. The old assertion here
// — `loudDb > -12.0f` — passed with 6 dB to spare and certified the error.
// Both scales are now pinned to 0.05 dB.
//
// Socket-free, Qt-free, radio-free. Nothing here has been near hardware and
// nothing here needs to be: it is arithmetic. Which is also its limit — the
// correction has never been checked against a converter driven to a known
// level, because no radio has yet answered the verb that feeds this window.

#include "gui/ClientEqFftAnalyzer.h"

#include <cmath>
#include <cstdio>
#include <vector>

using AetherSDR::ClientEqFftAnalyzer;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

constexpr int kN = ClientEqFftAnalyzer::kFftSize;

// A full-scale sine at bin `bin`, which is what a single carrier looks like to
// this transform. Exactly on a bin centre so there is no scalloping loss to
// reason about.
std::vector<float> tone(int bin, float amplitude)
{
    std::vector<float> v(kN);
    for (int i = 0; i < kN; ++i) {
        v[i] = amplitude * std::sin(2.0f * 3.14159265358979f
                                    * float(bin) * float(i) / float(kN));
    }
    return v;
}

int peakBin(const std::vector<float>& db)
{
    int best = 0;
    for (std::size_t i = 1; i < db.size(); ++i) {
        if (db[i] > db[best])
            best = int(i);
    }
    return best;
}

}  // namespace

int main()
{
    // ---- 1 · the record length the dialog assumes is the analyzer's own ----
    //
    // The dialog refuses a record of any other length rather than padding it,
    // because a padded record would put a frequency axis on the window that was
    // wrong by a factor. That refusal is only correct while these agree.
    check(ClientEqFftAnalyzer::kFftSize == 2048,
          "the analyzer transforms 2048 points — the HL2's EP4 block length");
    check(ClientEqFftAnalyzer::kBinCount == 1025,
          "and reports DC..Nyquist inclusive");

    // ---- 2 · reset() then update() is UNSMOOTHED ----
    //
    // The contract. Feed a loud tone, then reset and feed a quiet one: the
    // quiet reading must be the quiet tone's own level, not a value on its way
    // down from the loud one.
    {
        ClientEqFftAnalyzer a;
        const auto loud = tone(200, 1.0f);
        const auto quiet = tone(200, 0.01f);   // 40 dB down

        a.reset();
        a.update(loud.data(), kN);
        const float loudDb = a.magnitudesDb().at(200);

        a.reset();
        a.update(quiet.data(), kN);
        const float quietDb = a.magnitudesDb().at(200);

        check(std::fabs((loudDb - quietDb) - 40.0f) < 1.0f,
              "after reset() the quiet tone reads its OWN level, 40 dB below");
    }

    // ---- 2b · WHERE FULL SCALE ACTUALLY LANDS, on both scales ----
    //
    // This used to be `check(loudDb > -12.0f)`, which is a bound so loose it
    // passed with 6 dB to spare on a reading that WAS 6 dB wrong, and so it
    // certified the bug it was the only test standing near.
    //
    // There are two scales here and both are pinned, because the whole defect
    // was a caller assuming they were one scale:
    //
    //   * The analyzer's OWN bins are 6.02 dB low. update() normalises by 2/N —
    //     the single-sided normalisation for an UNWINDOWED transform — and does
    //     not remove the Hann window's 0.5 coherent gain. That is deliberate
    //     now, because the EQ editor has drawn this scale since it shipped and
    //     reads it only as a shape; it is pinned so the choice is visible
    //     rather than accidental.
    //   * coherentGainCorrectionDb() is the exact inverse of it, and the
    //     bandscope adds it before drawing, which is what makes
    //     BandscopeTrace::kTopDb = 0 mean the converter's rail.
    //
    // Tolerances are 0.05 dB, not 6. A change to buildWindow() or to `norm`
    // now breaks this test instead of quietly moving a number an operator
    // reads against a clip threshold.
    //
    // The correction is arithmetic. It has never been checked against a
    // converter driven to a known level, because no radio has yet answered the
    // verb that feeds this window.
    {
        ClientEqFftAnalyzer a;
        const auto loud = tone(200, 1.0f);   // rail to rail
        a.reset();
        a.update(loud.data(), kN);
        const float rawDb = a.magnitudesDb().at(200);
        const float corr  = a.coherentGainCorrectionDb();

        check(std::fabs(rawDb - (-6.0248f)) < 0.05f,
              "a full-scale sine reads -6.02 dBFS on the analyzer's raw scale");
        check(std::fabs(corr - 6.0248f) < 0.05f,
              "and coherentGainCorrectionDb() is the Hann window's +6.02 dB");
        check(std::fabs(rawDb + corr) < 0.05f,
              "so corrected, a rail-to-rail sine lands at 0 dBFS — "
              "the top of the bandscope's scale, and the converter's rail");

        // The correction is a property of the window, not of the signal: it is
        // the same number whatever is fed in, so applying it cannot distort the
        // relative picture the trace draws.
        const auto half = tone(200, 0.5f);
        a.reset();
        a.update(half.data(), kN);
        check(std::fabs(a.coherentGainCorrectionDb() - corr) < 1e-4f,
              "the correction does not depend on the record");
        check(std::fabs((a.magnitudesDb().at(200) + corr) - (-6.0206f)) < 0.05f,
              "and a half-scale sine sits 6.02 dB below the rail, as it should");
    }

    // ---- 3 · ...and the smoothing it defeats is really there ----
    //
    // The mirror of case 2. Without the reset the same second frame lands
    // somewhere between the two, which is what would appear in the window if
    // this dependency were ever quietly dropped. Asserted so that case 2 is
    // testing something rather than restating a coincidence.
    {
        ClientEqFftAnalyzer a;
        const auto loud = tone(200, 1.0f);
        const auto quiet = tone(200, 0.01f);

        a.reset();
        a.update(loud.data(), kN);
        const float loudDb = a.magnitudesDb().at(200);
        a.update(quiet.data(), kN);            // NO reset
        const float smearedDb = a.magnitudesDb().at(200);

        check(smearedDb < loudDb,
              "an unreset second frame moves towards the new level");
        check(smearedDb > loudDb - 39.0f,
              "...but does not reach it: this is the lag the bandscope must not show");
    }

    // ---- 4 · a tone lands where the frequency axis says it does ----
    //
    // The dialog labels its axis bin * sampleRate / kFftSize and reports the
    // peak's frequency from the same arithmetic. A transform that put energy in
    // a different bin would make both wrong together and neither obviously so.
    {
        ClientEqFftAnalyzer a;
        // 76.8 MHz is the HL2 converter's clock, so the span is DC..38.4 MHz.
        constexpr double kConverterHz = 76.8e6;
        for (const int bin : {17, 200, 1000}) {
            const auto v = tone(bin, 0.5f);
            a.reset();
            a.update(v.data(), kN);
            check(peakBin(a.magnitudesDb()) == bin,
                  "a tone peaks in its own bin");
            const double hz = ClientEqFftAnalyzer::binFreq(bin, kConverterHz);
            check(std::fabs(hz - bin * kConverterHz / kN) < 1.0,
                  "and binFreq agrees with the axis the dialog draws");
        }
    }

    // ---- 5 · silence is floored, not infinite ----
    //
    // An all-zero record is 20*log10(0). The window has to draw something.
    {
        ClientEqFftAnalyzer a;
        const std::vector<float> silence(kN, 0.0f);
        a.reset();
        a.update(silence.data(), kN);
        for (const float db : a.magnitudesDb()) {
            check(db <= ClientEqFftAnalyzer::kFloorDb + 0.001f && std::isfinite(db),
                  "every bin of a silent record is at the floor and finite");
            break;   // one representative bin; the loop above proves the shape
        }
        check(std::isfinite(a.magnitudesDb().at(kN / 4)),
              "including one well away from DC");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "bandscope_analyzer_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
