// aetherd ANAN P2 Phase 1b -- AnanBackend unit test.
//
// Tests the pieces testable without a live radio: capabilities() defaults,
// the mode-string parsing (and the two HERMES.md §16.7 regressions bare
// "CW" and "NFM" caused for the HL2 -- a mode name that appears in TCI's
// modulations_list but isn't mapped silently becomes USB), the CW BFO free
// function, and the passband-reset-only-on-an-ACTUAL-mode-change
// idempotence rule (mirrors Hl2Backend::setSliceMode's own behavior).
//
// Does NOT test connectRadio()'s socket behavior -- that is what commits
// 1-3's own tests (P2Protocol directly; P2Client/AnanRxDsp indirectly, by
// construction) already cover from the wire and DSP ends, plus radiocert rx
// on the bench once this backend is actually reachable from the GUI
// (commit 5).

#include "core/backends/anan/AnanBackend.h"

#include <QCoreApplication>

#include <cstdio>
#include <optional>

using namespace AetherSDR;
using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- mode string parsing ----
    {
        check(AnanBackend::modeFromString("usb") == WdspChannel::Mode::Usb, "lowercase usb parses");
        check(AnanBackend::modeFromString("LSB") == WdspChannel::Mode::Lsb, "LSB parses");
        check(AnanBackend::modeFromString("CWU") == WdspChannel::Mode::Cwu, "CWU parses");
        check(AnanBackend::modeFromString("CW") == WdspChannel::Mode::Cwu,
              "bare \"CW\" parses to Cwu -- the TCI/Flex spelling, HERMES §16.7's own regression");
        check(AnanBackend::modeFromString("CWL") == WdspChannel::Mode::Cwl, "CWL parses");
        check(AnanBackend::modeFromString("NFM") == WdspChannel::Mode::Fm,
              "NFM parses to Fm -- HERMES §16.7's other named regression");
        check(AnanBackend::modeFromString("FM") == WdspChannel::Mode::Fm, "FM parses");
        check(AnanBackend::modeFromString("AM") == WdspChannel::Mode::Am, "AM parses");
        check(AnanBackend::modeFromString("SAM") == WdspChannel::Mode::Sam, "SAM parses");
        check(AnanBackend::modeFromString("DIGU") == WdspChannel::Mode::Digu, "DIGU parses");
        check(AnanBackend::modeFromString("DIGL") == WdspChannel::Mode::Digl, "DIGL parses");
        check(AnanBackend::modeFromString("RTTY") == WdspChannel::Mode::Digu,
              "RTTY parses to Digu (WDSP has no dedicated RTTY demod) -- found by "
              "radiocert tune's mode-map stage falling through to the USB fallback below");
        check(AnanBackend::modeFromString("bogus") == WdspChannel::Mode::Usb,
              "unknown mode falls back to USB, not silently undefined behaviour");
    }

    // ---- default passbands ----
    {
        const auto [uLo, uHi] = AnanBackend::defaultPassbandForMode("USB");
        check(uLo == 100 && uHi == 2900, "USB default passband");
        const auto [lLo, lHi] = AnanBackend::defaultPassbandForMode("LSB");
        check(lLo == -2900 && lHi == -100, "LSB default passband is USB's mirror image");
        const auto [cLo, cHi] = AnanBackend::defaultPassbandForMode("CW");
        check(cLo == -250 && cHi == 250, "CW default passband is symmetric about the marker");
        const auto [aLo, aHi] = AnanBackend::defaultPassbandForMode("AM");
        check(aLo == -4000 && aHi == 4000, "AM default passband is symmetric (envelope detector)");
        const auto [dLo, dHi] = AnanBackend::defaultPassbandForMode("DIGU");
        const auto [rLo, rHi] = AnanBackend::defaultPassbandForMode("RTTY");
        check(dLo == 150 && dHi == 3000, "DIGU default passband");
        check(rLo == dLo && rHi == dHi,
              "RTTY shares DIGU's passband explicitly, not by coincidentally "
              "matching this function's own unknown-mode fallback");
    }

    // ---- CW BFO (HERMES.md §5: "CW has no BFO unless you build one") ----
    {
        check(AnanBackend::cwBfoOffsetHz("CWU", 600) == 600.0, "CWU BFO is +pitch");
        check(AnanBackend::cwBfoOffsetHz("CW", 600) == 600.0, "bare CW BFO is +pitch too");
        check(AnanBackend::cwBfoOffsetHz("CWL", 600) == -600.0, "CWL BFO is -pitch");
        check(AnanBackend::cwBfoOffsetHz("USB", 600) == 0.0, "non-CW mode has zero BFO");
        check(AnanBackend::cwBfoOffsetHz("AM", 600) == 0.0, "AM has zero BFO");
    }

    // ---- capabilities() ----
    {
        AnanBackend backend;
        const RadioCapabilities c = backend.capabilities();
        check(c.family == QStringLiteral("anan"), "family is anan");
        check(c.model == QStringLiteral("ANAN-G2"), "model is ANAN-G2");
        check(c.maxSlices == 1 && c.maxPanadapters == 1, "single slice, single pan in this phase");
        check(!c.canTransmit, "canTransmit is false -- P2Client has no PTT capability");
        check(c.hostModulates, "hostModulates true -- client-side WDSP, like the HL2");
        check(!c.radioOwnsDbmScale, "client computes the dBm scale");
        check(!c.hasTuner && !c.hasTunerMemories,
              "ANAN explicitly declares tuner matching and tuner memories absent");
        check(c.hasDdcPanEdgeRolloff,
              "hasDdcPanEdgeRolloff true -- ANAN's DDC has a real edge roll-off");
        check(c.tuningMinHz == 0.0 && c.tuningMaxHz == 0.0,
              "tuning range not reported -- no verified G2 range yet, not a guess");
        check(c.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
              "no restore support declared in this phase");
        check(c.sampleRatesHz.size() == 6, "six DDC rates advertised");
        check(!backend.isConnected(), "not connected before connectRadio() is ever called");
    }

    // ---- setSliceMode: passband reset only on an ACTUAL mode change ----
    {
        AnanBackend backend;
        std::optional<SliceDelta> last;
        QObject::connect(&backend, &IRadioBackend::sliceChanged,
                         [&last](int, const SliceDelta& d) { last = d; });

        backend.setSliceMode(0, QStringLiteral("USB"));
        check(last.has_value() && last->filterLow.value_or(-1) == 100
              && last->filterHigh.value_or(-1) == 2900,
              "USB sets the default passband");

        // Operator manually narrows the filter.
        backend.setSliceFilter(0, 300, 2700);
        check(last->filterLow.value_or(-1) == 300, "manual filter edit took");

        // Re-setting the SAME mode must NOT clobber the manual edit --
        // Hl2Backend::setSliceMode's own idempotence rule.
        backend.setSliceMode(0, QStringLiteral("USB"));
        check(last->filterLow.value_or(-1) == 300,
              "re-setting the same mode preserves the operator's manual filter edit");

        // Switching to a DIFFERENT mode DOES reset to that mode's default.
        backend.setSliceMode(0, QStringLiteral("LSB"));
        check(last->filterLow.value_or(1) == -2900 && last->filterHigh.value_or(1) == -100,
              "switching mode resets to the new mode's default passband");
        check(last->mode.value_or(QString()) == QStringLiteral("LSB"),
              "mode field reflects the new mode");
    }

    // ---- setSliceAgc: live state, not just a fire-and-forget push ----
    // Regression test for a real (fixed) bug: beginRateChange() used to
    // refresh only .inputSampleRateHz in m_pendingDspConfig, so a rate
    // change silently reverted AGC (and mode/filter) to whatever they were
    // at connectRadio() time. m_agcMode/m_agcCeilingDb are what
    // beginRateChange() now reads instead -- this pins that setSliceAgc()
    // actually populates them, since there is no live-radio path to test
    // beginRateChange() itself against here.
    {
        AnanBackend backend;
        check(backend.agcModeForTest() == 3 && backend.agcCeilingDbForTest() == 60.0,
              "AGC defaults match connectRadio()'s own connect-time defaults");

        backend.setSliceAgc(0, QStringLiteral("fast"), 50);
        check(backend.agcModeForTest() == 4, "\"fast\" maps to WDSP AGC mode 4");
        check(backend.agcCeilingDbForTest() == 30.0,
              "50 operator units -> 30 dB ceiling (0.6 dB/unit)");

        backend.setSliceAgc(0, QStringLiteral("off"), 100);
        check(backend.agcModeForTest() == 0, "\"off\" maps to WDSP AGC mode 0");
        check(backend.agcCeilingDbForTest() == 60.0, "100 operator units -> 60 dB ceiling");
    }

    // ---- nearestDdc0RateKsps: nearest by RATIO, not linear distance ----
    // Mirrors hl2_backend_test's own span-snap table -- HERMES.md §15.1:
    // these rates are octave-spaced and zoom is multiplicative, so a plain
    // linear "closest wins" search is provably wrong for a request between
    // the geometric and arithmetic mean of two adjacent rates. ANAN's first
    // four rates (48/96/192/384) are the exact same numbers as the HL2's
    // own four, so the same 140 -> 192 case (the one row that actually
    // tells ratio and linear distance apart) applies unchanged; extended
    // here with the same shape of case for the two rates the HL2 doesn't
    // have (384/768/1536).
    {
        struct RateCase {
            int requestedKsps;
            int expectKsps;
            const char* what;
        };
        const RateCase cases[] = {
            {384, 384, "an exact rate is taken exactly"},
            {100, 96, "100 ksps snaps DOWN to 96 ksps, not up to 192 ksps"},
            // THE case that pins ratio-nearest rather than linear-nearest --
            // see hl2_backend_test's own identical row for the exact math
            // (geometric mean 135.8, arithmetic mean 144, so 140 falls on
            // opposite sides of the two rules). Without this row the log()
            // could be deleted and the suite would stay green.
            {140, 192, "140 ksps snaps UP to 192 ksps -- nearest by RATIO, "
                       "not by linear distance"},
            // Same shape one octave up: between 384 and 768, geometric mean
            // is ~543, arithmetic mean is 576. 560 falls in that gap.
            {560, 768, "560 ksps snaps UP to 768 ksps -- ratio, not linear "
                       "distance, one octave up from the 140 case"},
            {48, 48, "the narrowest request reaches 48 ksps"},
            {1536, 1536, "the widest request reaches 1536 ksps"},
            {5400, 1536, "a request past the widest rate clamps to 1536 ksps"},
            {0, 48, "zero floors at 48 ksps rather than underflowing log()"},
        };
        for (const auto& c : cases) {
            const int got = AnanBackend::nearestDdc0RateKsps(c.requestedKsps);
            check(got == c.expectKsps, c.what);
            if (got != c.expectKsps) {
                std::fprintf(stderr, "  requested %d ksps, expected %d, got %d\n",
                             c.requestedKsps, c.expectKsps, got);
            }
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
