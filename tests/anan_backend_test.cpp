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

#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/backends/anan/AnanBackend.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <limits>
#include <memory>
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
    // The child inherits the parent's isolated store to prove disk persistence.
    const bool restoreChild = argc == 2 && QByteArray(argv[1]) == "--restore-check";
    std::unique_ptr<TestSettingsProfile> profile;
    if (!restoreChild) {
        profile = std::make_unique<TestSettingsProfile>(QStringLiteral("anan-backend-test"));
        if (!profile->isValid()) {
            return 1;
        }
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    if (restoreChild) {
        AnanBackend backend;
        const RadioSettingsScope radioA(QStringLiteral("anan"), QStringLiteral("G2-A"));
        backend.applyRestoredState(RadioStateMemory::load(radioA, backend.capabilities()));
        const QJsonObject gain = backend.currentOperatingState().extension
                                     .value(QStringLiteral("rfGain")).toObject();
        check(gain.value(QStringLiteral("adc0AttenuationDb")).toInt() == 12
                  && gain.value(QStringLiteral("adc1AttenuationDb")).toInt() == 20,
              "both ADC attenuations survive a process boundary");
        return g_failures == 0 ? 0 : 1;
    }

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
        check(!c.canCreateSlices, "ANAN fixed receiver does not expose independent slice creation");
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
        check(c.backendPanAveraging.has_value()
                  && c.backendPanAveraging->msPerAverageStep == 10,
              "backendPanAveraging engaged, 10 ms per FFT AVG step -- the WDSP analyzer averages");
        check(c.tuningMinHz == 0.0 && c.tuningMaxHz == 0.0,
              "tuning range not reported -- no verified G2 range yet, not a guess");
        check(c.clientSettingsDomains == RadioCapabilities::ClientSettingsDomain::RfGain,
              "only RF gain restore is declared");
        check(c.sampleRatesHz.size() == 6, "six DDC rates advertised");
        check(c.hasHostNoiseBlanker,
              "hasHostNoiseBlanker true -- WDSP ANB runs in AnanRxDsp, so the "
              "VFO's NB button has something to drive");
        check(!backend.isConnected(), "not connected before connectRadio() is ever called");
    }

    // ---- setSliceNoiseBlanker: live state for connect and rate change ----
    // Same reason as the AGC block below: connectRadio() and
    // beginRateChange() build the DSP config from these members.
    {
        AnanBackend backend;
        check(!backend.noiseBlankerOnForTest() && backend.noiseBlankerLevelForTest() == 50,
              "noise blanker defaults match AnanRxDsp::Config's");
        backend.setSliceNoiseBlanker(0, true, 70);
        check(backend.noiseBlankerOnForTest() && backend.noiseBlankerLevelForTest() == 70,
              "setSliceNoiseBlanker() stores the operator's NB state");
        backend.setSliceNoiseBlanker(0, false, -5);
        check(!backend.noiseBlankerOnForTest() && backend.noiseBlankerLevelForTest() == 0,
              "NB off stored, level clamped to 0");
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

    {
        // droopStatus() reports the SEEDED DEFAULTS alongside a sweep's
        // measured tables, so the page stops saying "nothing measured" while a
        // shipped curve is live. The invariant worth pinning is the other
        // direction: a rate must claim NO correction until connectRadio() has
        // actually seeded one. A disconnected backend reporting six live
        // defaults would be the same lie the change set out to fix.
        AnanBackend backend;
        const QVariantMap status = backend.droopStatus();
        const QVariantList corrections =
            status.value(QStringLiteral("corrections")).toList();
        check(corrections.isEmpty(),
              "a disconnected backend claims no droop correction for any rate");
        if (!corrections.isEmpty()) {
            std::fprintf(stderr, "  expected no corrections, got %lld\n",
                         static_cast<long long>(corrections.size()));
        }
        check(!status.value(QStringLiteral("hasResult")).toBool(),
              "a disconnected backend stages no sweep result");
    }

    {
        // RF Gain drives the G2's step attenuator: gain -12 dB = 12 dB of
        // attenuation, clamped to the spec's 0-31 dB, and echoed back as the
        // gain actually applied.
        AnanBackend backend;
        test::SeamThreadAffinityProbe probe(&backend);
        test::attachAllSeamSignals(probe);
        QSignalSpy changes(&backend, &IRadioBackend::operatingStateChanged);
        QSignalSpy spy(&backend, &IRadioBackend::panRfGainChanged);
        backend.setPanRfGain(QStringLiteral("anan-0"), -12);
        check(backend.attenuationDbForTest() == 12, "RF gain -12 dB -> 12 dB attenuation");
        check(spy.count() == 1 && spy.at(0).at(1).toInt() == -12,
              "the applied gain (-12) is echoed on panRfGainChanged");
        backend.setPanRfGain(QStringLiteral("anan-0"), -40);
        check(backend.attenuationDbForTest() == 31, "below -31 dB clamps to 31 dB attenuation");
        check(spy.count() == 2 && spy.at(1).at(1).toInt() == -31,
              "the clamped gain (-31), not the request, is echoed");
        backend.setPanRfGain(QStringLiteral("anan-0"), 8);
        check(backend.attenuationDbForTest() == 0, "positive gain clamps to 0 dB attenuation");
        backend.setPanRfGain(QStringLiteral("anan-0"), std::numeric_limits<int>::min());
        check(backend.attenuationDbForTest() == 31, "INT_MIN gain clamps to maximum attenuation");
        backend.setPanRfGain(QStringLiteral("anan-0"), std::numeric_limits<int>::max());
        check(backend.attenuationDbForTest() == 0, "INT_MAX gain clamps to zero attenuation");
        check(changes.count() == 5, "RF gain changes notify the operating-state capture pipeline");
        check(probe.violations().isEmpty(), "RF gain and capture signals stay on the owner thread");
    }

    {
        // S-meter: WDSP's dBFS reading is published as SLC:LEVEL in dBm with
        // deskHPSDR's 0 dB ANAN offset, the first reading at once, later ones
        // smoothed with SMeterSmoother's ballistics.
        AnanBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::meterUpdate);
        backend.feedMeterForTest(-73.0f);
        check(spy.count() == 1, "first S-meter reading is published at once");
        if (spy.count() == 1) {
            check(spy.at(0).at(0).toString() == QStringLiteral("SLC:LEVEL"),
                  "S-meter is published as SLC:LEVEL");
            check(qAbs(spy.at(0).at(1).toDouble() - (-73.0)) < 1e-9,
                  "-73 dBFS reads -73 dBm (0 dB offset, as deskHPSDR ships for ANAN)");
        }
        check(qAbs(backend.sMeterDbmForTest() - (-73.0)) < 1e-9,
              "the first reading is taken whole, not smoothed against a zero start");

        backend.feedMeterForTest(-53.0f);
        // Whether that second reading was published is NOT asserted here: it
        // would ride on fewer than 100 ms of wall clock passing between two
        // calls. The publish tick is pinned deterministically, against an
        // injected clock, in wdsp_smeter_test.
        // Ballistics pinned on the smoothed value rather than on whatever the
        // 100 ms tick happened to publish: attack 0.5 on a rise, decay 0.15 on
        // a fall, which is what "HL2's ballistics" means here. Replacing the
        // EMA with a plain assignment moves both numbers, so the claim is now
        // covered rather than merely stated.
        check(qAbs(backend.sMeterDbmForTest() - (-63.0)) < 1e-9,
              "a rise is smoothed with attack 0.5: -73 then -53 reads -63 dBm");
        backend.feedMeterForTest(-83.0f);
        check(qAbs(backend.sMeterDbmForTest() - (-66.0)) < 1e-9,
              "a fall is smoothed with decay 0.15: -66 dBm, so the needle falls "
              "more slowly than it rises");
    }

    {
        AnanBackend backend;
        test::SeamThreadAffinityProbe probe(&backend);
        test::attachAllSeamSignals(probe);
        QSignalSpy meter(&backend, &IRadioBackend::meterUpdate);
        backend.setPanRfGain(QStringLiteral("anan-0"), -12);
        backend.feedMeterForTest(-85.0f);
        check(meter.count() == 1 && qAbs(meter.at(0).at(1).toDouble() + 73.0) < 1e-9,
              "12 dB attenuation is added back to the first S-meter publication");
        backend.feedMeterForTest(-65.0f);
        check(qAbs(backend.sMeterDbmForTest() + 63.0) < 1e-9,
              "attenuation compensation precedes attack smoothing");
        backend.setPanRfGain(QStringLiteral("anan-0"), -31);
        backend.feedMeterForTest(-114.0f);
        check(qAbs(backend.sMeterDbmForTest() + 66.0) < 1e-9,
              "changed attenuation compensation precedes decay smoothing");
        check(probe.violations().isEmpty(), "compensated meters stay on the owner thread");
    }

    {
        AnanBackend backend;
        const RadioCapabilities caps = backend.capabilities();
        const RadioSettingsScope radioA(QStringLiteral("anan"), QStringLiteral("G2-A"));
        const RadioSettingsScope radioB(QStringLiteral("anan"), QStringLiteral("G2-B"));
        RestoredRadioState state;
        state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("adc0AttenuationDb"), 7},
            {QStringLiteral("adc1AttenuationDb"), 20}}}};
        backend.applyRestoredState(state);
        backend.setPanRfGain(QStringLiteral("anan-0"), -12);
        check(RadioStateMemory::store(radioA, caps, backend.currentOperatingState()),
              "ANAN capture persists through OperatingState");
        backend.applyRestoredState(RadioStateMemory::load(radioB, caps));
        const QJsonObject emptyGain = backend.currentOperatingState().extension
                                          .value(QStringLiteral("rfGain")).toObject();
        check(emptyGain.value(QStringLiteral("adc0AttenuationDb")).toInt() == 0
                  && emptyGain.value(QStringLiteral("adc1AttenuationDb")).toInt() == 0,
              "a radio with no document resets both ADCs rather than inheriting radio A");
        backend.setPanRfGain(QStringLiteral("anan-0"), -5);
        check(RadioStateMemory::store(radioB, caps, backend.currentOperatingState()),
              "radio B gets its own operating-state document");
        backend.applyRestoredState(RadioStateMemory::load(radioA, caps));
        const QJsonObject gain = backend.currentOperatingState().extension
                                     .value(QStringLiteral("rfGain")).toObject();
        check(gain.value(QStringLiteral("adc0AttenuationDb")).toInt() == 12
                  && gain.value(QStringLiteral("adc1AttenuationDb")).toInt() == 20,
              "radio A preserves both ADC values independently of radio B");
        check(!AppSettings::instance().contains(QStringLiteral("Anan")),
              "live attenuation never writes the global connect-options document");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--restore-check")});
        const bool finished = child.waitForFinished(30000);
        const QByteArray output = child.readAllStandardOutput() + child.readAllStandardError();
        std::fprintf(stderr, "%s", output.constData());
        check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
              "a fresh process restores the persisted ADC values");

        state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("adc0AttenuationDb"), 42},
            {QStringLiteral("adc1AttenuationDb"), -5}}}};
        backend.applyRestoredState(state);
        const QJsonObject clamped = backend.currentOperatingState().extension
                                        .value(QStringLiteral("rfGain")).toObject();
        check(clamped.value(QStringLiteral("adc0AttenuationDb")).toInt() == 31
                  && clamped.value(QStringLiteral("adc1AttenuationDb")).toInt() == 0,
              "restored attenuation is clamped at both protocol bounds");
        state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("adc0AttenuationDb"), QStringLiteral("bad")},
            {QStringLiteral("adc1AttenuationDb"), 1e30}}}};
        backend.applyRestoredState(state);
        const QJsonObject malformed = backend.currentOperatingState().extension
                                          .value(QStringLiteral("rfGain")).toObject();
        check(malformed.value(QStringLiteral("adc0AttenuationDb")).toInt() == 0
                  && malformed.value(QStringLiteral("adc1AttenuationDb")).toInt() == 0,
              "malformed attenuation restores both zero defaults");
        backend.applyRestoredState({});
        check(backend.attenuationDbForTest() == 0, "empty restore clears live attenuation");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
