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
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>

namespace AetherSDR::anan {
// AnanBackend's friend: drives the client-snapshot handler with no client and
// no socket, which is the only deterministic way to reach LinkStats::alive.
class AnanLinkStatsTestAccess {
public:
    static void setConnected(AnanBackend& backend, bool connected)
    {
        backend.m_connected = connected;
    }
    static void snapshot(AnanBackend& backend, quint64 rxBytes)
    {
        P2Client::LinkCounters counters;
        counters.rxBytes = rxBytes;
        backend.onLinkCounters(counters);
    }
};
}  // namespace AetherSDR::anan

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

namespace {
// Reads as connected so RadioModel dispatches slice edits to it; no socket.
class ConnectedAnan : public AnanBackend {
public:
    bool isConnected() const override { return true; }
};
}  // namespace

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
        check(backend.restoredFrequencyHzForTest() == 7'074'000.0,
              "the dial survives a process boundary");
        check(backend.restoredModeForTest() == QStringLiteral("DIGU"),
              "the mode survives a process boundary too");
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

    // ---- restore-boundary vocabulary ----
    // modeFromString() answers USB for anything unknown, so it cannot judge.
    {
        for (const char* m : {"LSB", "USB", "DSB", "CWU", "CW", "CWL", "FM", "NFM",
                              "AM", "DIGU", "DIGL", "RTTY", "SAM", "DRM", "WBFM", "WFM"}) {
            check(AnanBackend::isKnownModeString(QString::fromLatin1(m)),
                  "every spelling modeFromString() maps is accepted at restore");
        }
        check(AnanBackend::isKnownModeString("usb"), "the accept test is case-insensitive");
        check(!AnanBackend::isKnownModeString("bogus"),
              "a mode modeFromString() does not map is REFUSED, not silently demodulated as SSB");
        check(!AnanBackend::isKnownModeString(""), "an empty mode is not a mode");
    }

    // ---- restorable frequency: bounded by the DDS encoding, Nyquist ----
    {
        check(AnanBackend::isRestorableFrequencyHz(14'175'000.0), "20 m phone is restorable");
        check(AnanBackend::isRestorableFrequencyHz(1.0), "just above zero is restorable");
        check(!AnanBackend::isRestorableFrequencyHz(0.0),
              "zero is the not-restored sentinel, never a frequency");
        check(!AnanBackend::isRestorableFrequencyHz(-1.0), "a negative frequency is refused");
        check(!AnanBackend::isRestorableFrequencyHz(kDspClockHz / 2.0),
              "Nyquist itself is refused -- it aliases rather than tuning");
        check(!AnanBackend::isRestorableFrequencyHz(200'000'000.0),
              "far past Nyquist is refused, NOT clamped: the document is wrong, not nearly right");
        check(!AnanBackend::isRestorableFrequencyHz(
                  std::numeric_limits<double>::quiet_NaN()),
              "NaN is refused");
        check(!AnanBackend::isRestorableFrequencyHz(
                  std::numeric_limits<double>::infinity()),
              "infinity is refused");
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
                  && !c.backendPanAveraging->clientPersistsAveraging
                  && c.backendPanAveraging->averageDescription.isEmpty()
                  && c.backendPanAveraging->weightedDescription.isEmpty()
                  && c.backendPanAveraging->msPerAverageStep == 10,
              "backendPanAveraging engaged, 10 ms per FFT AVG step -- the WDSP analyzer averages");
        check(c.tuningMinHz == 0.0 && c.tuningMaxHz == 0.0,
              "tuning range not reported -- no verified G2 range yet, not a guess");
        check(c.clientSettingsDomains
                  == (RadioCapabilities::ClientSettingsDomain::RfGain
                      | RadioCapabilities::ClientSettingsDomain::Tuning
                      | RadioCapabilities::ClientSettingsDomain::ReceiveOutputLevel),
              "RF gain, tuning AND radio output level restore are declared -- "
              "this radio reports none of them, so the client is its only memory");
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
        backend.setSliceNoiseBlanker(0, AetherSDR::NoiseBlankerKind::Impulse, 70,
                                     AetherSDR::NoiseBlankerFill::Zero);
        check(backend.noiseBlankerOnForTest() && backend.noiseBlankerLevelForTest() == 70,
              "setSliceNoiseBlanker() stores the operator's NB state");
        backend.setSliceNoiseBlanker(0, AetherSDR::NoiseBlankerKind::Off, -5,
                                     AetherSDR::NoiseBlankerFill::Zero);
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

    // ---- AGC-T knob scale: -20..120 dB, deskHPSDR's and Thetis's range ----
    {
        const auto approx = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        check(approx(AnanBackend::agcKnobDb(0), -20.0), "knob 0 is -20 dB");
        check(approx(AnanBackend::agcKnobDb(50), 50.0), "knob 50 is 50 dB (1.4 dB per step)");
        check(approx(AnanBackend::agcKnobDb(100), 120.0), "knob 100 is 120 dB");
        check(approx(AnanBackend::agcKnobDb(-5), -20.0) && approx(AnanBackend::agcKnobDb(150), 120.0),
              "the knob is clamped to 0..100 before it is mapped");
        check(approx(AnanBackend::agcKnobDb(AnanBackend::kDefaultAgcThreshold), 79.4),
              "the default AGC gain is 79.4 dB, the step nearest deskHPSDR's 80");
        check(approx(AnanBackend::agcKnobDb(AnanBackend::kDefaultAgcOffLevel), 20.6),
              "the default AGC-off gain is 20.6 dB, the step nearest Thetis's 20");
    }

    // ---- setSliceAgc / requestSliceAgc: live state, published ----
    // connectRadio() and beginRateChange() both build the DSP config from these
    // members, so they must hold what the operator set. Publishing them is what
    // keeps the knob showing what the DSP runs (#5988).
    {
        AnanBackend backend;
        std::optional<SliceDelta> last;
        QObject::connect(&backend, &IRadioBackend::sliceChanged,
                         [&last](int, const SliceDelta& d) { last = d; });
        check(backend.agcModeForTest() == 3
                  && backend.agcMaxGainDbForTest() == AnanBackend::agcKnobDb(AnanBackend::kDefaultAgcThreshold)
                  && backend.agcFixedGainDbForTest() == AnanBackend::agcKnobDb(AnanBackend::kDefaultAgcOffLevel),
              "AGC starts at medium with the default gain and AGC-off gain");

        backend.setSliceAgc(0, QStringLiteral("fast"), 50);
        check(backend.agcModeForTest() == 4, "\"fast\" maps to WDSP AGC mode 4");
        check(backend.agcMaxGainDbForTest() == AnanBackend::agcKnobDb(50),
              "the threshold sets the AGC gain through the knob scale");
        check(last.has_value() && last->agcMode.value_or(QString()) == QStringLiteral("fast")
                  && last->agcThreshold.value_or(-1) == 50
                  && last->agcOffLevel.value_or(-1) == AnanBackend::kDefaultAgcOffLevel,
              "an AGC change publishes the mode, threshold and AGC-off level it applied");

        backend.setSliceAgc(0, QStringLiteral("off"), 140);
        check(backend.agcModeForTest() == 0, "\"off\" maps to WDSP AGC mode 0");
        check(last->agcMode.value_or(QString()) == QStringLiteral("off")
                  && last->agcThreshold.value_or(-1) == 100,
              "a threshold above 100 is clamped, and the clamped value is published");

        last.reset();
        backend.requestSliceAgc(0, {SliceAgcRequest::Field::OffLevel, QStringLiteral("off"),
                                    100, 40, SliceAgcRequest::Origin::Operator});
        check(backend.agcFixedGainDbForTest() == AnanBackend::agcKnobDb(40),
              "the AGC-off level sets the fixed gain through the knob scale");
        check(backend.agcModeForTest() == 0 && backend.agcMaxGainDbForTest() == AnanBackend::agcKnobDb(100),
              "an AGC-off level edit leaves the mode and threshold alone");
        check(last.has_value() && last->agcOffLevel.value_or(-1) == 40,
              "an AGC-off level edit is published");

        backend.requestSliceAgc(0, {SliceAgcRequest::Field::Threshold, QStringLiteral("slow"),
                                    30, 40, SliceAgcRequest::Origin::Operator});
        check(backend.agcModeForTest() == 2 && backend.agcMaxGainDbForTest() == AnanBackend::agcKnobDb(30),
              "a threshold request still reaches setSliceAgc() with its mode");
    }

    // ---- applyLiveRxState(): the receive state a connect or rate change builds ----
    // connectRadio() and beginRateChange() both fill the DSP config with it.
    {
        AnanBackend backend;
        backend.setSliceMode(0, QStringLiteral("LSB"));
        backend.setSliceAgc(0, QStringLiteral("slow"), 40);
        backend.requestSliceAgc(0, {SliceAgcRequest::Field::OffLevel, QStringLiteral("slow"),
                                    40, 60, SliceAgcRequest::Origin::Operator});
        backend.setSliceNoiseBlanker(0, AetherSDR::NoiseBlankerKind::Impulse, 70,
                                     AetherSDR::NoiseBlankerFill::Interpolate);
        const AnanRxDsp::Config config = backend.liveRxConfigForTest();
        check(config.mode == WdspChannel::Mode::Lsb && config.filterLowHz == -2900.0
                  && config.filterHighHz == -100.0,
              "the DSP config carries the live mode and passband");
        check(config.agcMode == 2 && config.maximumAgcGainDb == AnanBackend::agcKnobDb(40)
                  && config.agcFixedGainDb == AnanBackend::agcKnobDb(60),
              "and the live AGC mode, AGC gain and AGC-off gain");
        check(static_cast<int>(config.noiseBlanker)
                      == static_cast<int>(AetherSDR::NoiseBlankerKind::Impulse)
                  && config.noiseBlankerLevel == 70
                  && static_cast<int>(config.noiseBlankerFill)
                      == static_cast<int>(AetherSDR::NoiseBlankerFill::Interpolate),
              "and the live noise blanker");
    }

    // ---- the knob shows what the DSP runs, through RadioModel (#5988) ----
    // SliceModel's own default is 65. The backend's published AGC replaces it,
    // so a touch or bookmark recall starts from what the DSP runs.
    {
        RadioModel radio;
        auto owned = std::make_unique<ConnectedAnan>();
        ConnectedAnan* backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("anan"));
        backend->setSliceFilter(0, 100, 2900);   // the first publication creates the slice
        SliceModel* slice = radio.slice(0);
        check(slice != nullptr, "ANAN publication creates the slice");
        if (slice) {
            check(slice->agcMode() == QStringLiteral("med")
                      && slice->agcThreshold() == AnanBackend::kDefaultAgcThreshold
                      && slice->agcOffLevel() == AnanBackend::kDefaultAgcOffLevel,
                  "the knob shows the backend's AGC, not the slice model's own defaults");
            slice->setAgcThreshold(60);
            check(backend->agcMaxGainDbForTest() == AnanBackend::agcKnobDb(60),
                  "a knob edit reaches the backend");
            slice->setAgcMode(QStringLiteral("off"));
            slice->setAgcOffLevel(35);
            check(backend->agcModeForTest() == 0
                      && backend->agcFixedGainDbForTest() == AnanBackend::agcKnobDb(35),
                  "with AGC off the knob sets the AGC-off gain");
            check(slice->agcThreshold() == 60 && slice->agcOffLevel() == 35,
                  "the published values match what the operator set");
        }
    }

    // ---- nearestDdc0RateKsps: nearest by RATIO, not linear distance ----
    // Mirrors hl2_backend_seam_test's span-snap table -- HERMES.md §15.1:
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
            // see hl2_backend_seam_test's identical row for the exact math
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
        // Set the dial BEFORE radio A's store so the child reads it off disk.
        backend.setSliceFrequency(0, 7'074'000.0);
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(backend.currentOperatingState().rfFrequencyHz == 7'074'000.0
                  && backend.currentOperatingState().mode == QStringLiteral("DIGU"),
              "capture reports the LIVE dial, not the restored one");
        // The level and its signal: nothing else tells the engine it moved.
        QSignalSpy levelChanges(&backend, &IRadioBackend::operatingStateChanged);
        backend.setLineoutGain(35);
        check(backend.currentOperatingState().receiveOutputLevelPct == 35,
              "capture reports the live radio output level");
        check(levelChanges.count() == 1,
              "a changed output level announces itself for capture");
        backend.setLineoutGain(35);
        check(levelChanges.count() == 1,
              "setting the SAME level again announces nothing -- this arrives "
              "per step while the operator drags the slider");
        check(RadioStateMemory::store(radioA, caps, backend.currentOperatingState()),
              "ANAN capture persists through OperatingState");
        backend.applyRestoredState(RadioStateMemory::load(radioB, caps));
        const QJsonObject emptyGain = backend.currentOperatingState().extension
                                          .value(QStringLiteral("rfGain")).toObject();
        check(emptyGain.value(QStringLiteral("adc0AttenuationDb")).toInt() == 0
                  && emptyGain.value(QStringLiteral("adc1AttenuationDb")).toInt() == 0,
              "a radio with no document resets both ADCs rather than inheriting radio A");
        check(backend.restoredFrequencyHzForTest() == 0.0
                  && backend.restoredModeForTest().isEmpty(),
              "a radio with no document clears the restored dial too -- radio A's "
              "frequency must not be carried onto radio B by a reused backend");
        // Two G2s on one profile: radio B starts at the default, not at 35.
        check(backend.lineoutGainPercentForTest() == 50,
              "a radio with no document starts at the default output level, "
              "not at the previous radio's");
        backend.setPanRfGain(QStringLiteral("anan-0"), -5);
        backend.setLineoutGain(20);
        check(RadioStateMemory::store(radioB, caps, backend.currentOperatingState()),
              "radio B gets its own operating-state document");
        backend.applyRestoredState(RadioStateMemory::load(radioA, caps));
        const QJsonObject gain = backend.currentOperatingState().extension
                                     .value(QStringLiteral("rfGain")).toObject();
        check(gain.value(QStringLiteral("adc0AttenuationDb")).toInt() == 12
                  && gain.value(QStringLiteral("adc1AttenuationDb")).toInt() == 20,
              "radio A preserves both ADC values independently of radio B");
        check(backend.lineoutGainPercentForTest() == 35,
              "radio A's output level comes back as 35 after radio B stored 20 -- "
              "each radio keeps its own");
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
        // Tuning is refused, not clamped, field by field.
        {
            RestoredRadioState bad;
            bad.rfFrequencyHz = 900'000'000.0;
            bad.mode = QStringLiteral("bogus");
            backend.applyRestoredState(bad);
            check(backend.restoredFrequencyHzForTest() == 0.0,
                  "an out-of-range stored frequency is dropped, not clamped to Nyquist");
            check(backend.restoredModeForTest().isEmpty(),
                  "an unmappable stored mode is dropped rather than demodulating as SSB");
            RestoredRadioState half;
            half.rfFrequencyHz = 10'000'000.0;
            half.mode = QStringLiteral("nonsense");
            backend.applyRestoredState(half);
            check(backend.restoredFrequencyHzForTest() == 10'000'000.0
                      && backend.restoredModeForTest().isEmpty(),
                  "a bad mode does not take a good frequency down with it");
            RestoredRadioState cased;
            cased.rfFrequencyHz = 14'175'000.0;
            cased.mode = QStringLiteral("digu");
            backend.applyRestoredState(cased);
            check(backend.restoredModeForTest() == QStringLiteral("DIGU"),
                  "an accepted mode is normalised to the spelling the rest of the backend uses");
        }
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

    // ---- the radio's own output level is NOT the per-slice stage ----
    //
    // Three levels exist and they must stay three: per-slice ("how loud is this
    // receiver"), the client master ("how loud is this computer"), and lineout
    // ("how loud is the radio"). The bug that made this necessary was reaching for
    // the master slider and hearing the radio blast, because only the first of the
    // three reached the radio at all.
    {
        AnanBackend backend;
        std::optional<SliceDelta> last;
        QObject::connect(&backend, &IRadioBackend::sliceChanged,
                         [&last](int, const SliceDelta& d) { last = d; });

        // Lineout is the RADIO's level, so it must not appear in the slice's
        // published observation -- that field is the per-slice gain, and a lineout
        // change writing it would make the receiver's own fader lie.
        backend.setSliceAudioGain(0, 80);
        last.reset();
        backend.setLineoutGain(30);
        check(!last.has_value(),
              "a lineout change publishes no slice state, being a different level");
        backend.setSliceAudioGain(0, 80);
        check(!last.has_value(), "and it did not disturb the slice's own gain");

        backend.setLineoutMute(true);
        check(!last.has_value(), "a lineout mute is not the slice's mute either");
        check(backend.lineoutMutedForTest(), "the lineout mute is stored");

        // Clamped at this seam, like the per-slice gain. An earlier revision
        // asserted this with check(true, ...) -- a test that could not fail, which
        // is worse than no test because it reads as coverage.
        check(backend.lineoutGainPercentForTest() == 30, "the lineout gain is stored");
        backend.setLineoutGain(400);
        check(backend.lineoutGainPercentForTest() == 100, "lineout gain above 100 clamps");
        backend.setLineoutGain(-9);
        check(backend.lineoutGainPercentForTest() == 0, "lineout gain below 0 clamps");
    }

    // ---- the receiver's audio stage: applied AND published ----
    //
    // Publishing is not cosmetic here. ModelReceiveControlTarget refuses an audio
    // operation unless the capability is declared AND the slice carries an
    // observation, so a backend that applied gain without echoing it would offer
    // the control and then reject every use of it with "capability.unavailable".
    {
        AnanBackend backend;
        std::optional<SliceDelta> last;
        QObject::connect(&backend, &IRadioBackend::sliceChanged,
                         [&last](int, const SliceDelta& d) { last = d; });

        backend.setSliceAudioGain(0, 70);
        check(last.has_value() && last->audioGain.value_or(-1) == 70,
              "a gain change publishes the applied gain");
        backend.setSliceAudioMute(0, true);
        check(last->audioMute.value_or(false), "a mute publishes the applied mute");
        backend.setSliceAudioMute(0, false);
        check(last->audioMute.has_value() && !last->audioMute.value(),
              "an unmute publishes false, rather than dropping the observation");

        // Out of range is clamped at the seam, not passed through to the audio.
        backend.setSliceAudioGain(0, 500);
        check(last->audioGain.value_or(-1) == 100, "gain above 100 clamps to 100");
        backend.setSliceAudioGain(0, -20);
        check(last->audioGain.value_or(-1) == 0, "gain below 0 clamps to 0");

        // An idempotent set must not republish -- emitSliceState() is how the NB
        // and mode fields reach the model too, so a setter that fired on every
        // repeat would churn the whole slice on a slider the operator is holding
        // still.
        backend.setSliceAudioGain(0, 55);
        last.reset();
        backend.setSliceAudioGain(0, 55);
        check(!last.has_value(), "re-setting the same gain publishes nothing");
        backend.setSliceAudioMute(0, false);
        check(!last.has_value(), "re-setting the same mute publishes nothing");

        // Balance is applied but deliberately NOT published: SliceDelta carries no
        // field for it, so there is no observation to echo. Pinning it here means a
        // future field cannot be added without this test being revisited.
        backend.setSliceAudioPan(0, 0);
        check(!last.has_value(), "balance publishes nothing, having no delta field");
    }

    // ---- PA telemetry capability: the readout is withdrawn, not blanked ----
    {
        AnanBackend backend;
        const auto caps = backend.capabilities();
        check(caps.paTelemetryAudit.has_value(),
              "ANAN declares a PA telemetry audit");
        check(caps.paTelemetryAudit && caps.paTelemetryAudit->temperatureAbsent,
              "the audit states PA temperature is ABSENT FROM THE PROTOCOL -- the"
              " claim the status bar withdraws its readout on");
        check(!caps.hasPaTemperatureTelemetry,
              "and no temperature is claimed as received");
        // The radio DOES send a supply count, but no known scale fits it (a
        // 13.8 V rail read as ~40 V on a G2 bench), so the row stays withdrawn.
        check(!caps.hasSupplyVoltageTelemetry,
              "no supply voltage is claimed while no scale for its counts is known");
        check(!caps.hasPaCurrentTelemetry,
              "no PA drain current is claimed");
    }

    // ---- LinkStats: silence before the first snapshot, never a zeroed one ----
    {
        AnanBackend backend;
        const auto idle = backend.linkStats();
        check(!idle.reported,
              "an unconnected backend reports NO transport, so the consumer keeps"
              " its own source instead of being handed zeros");
        check(idle.rttMs < 0 && idle.jitterMs < 0 && idle.gapMs < 0
                  && idle.gapMaxMs < 0,
              "every timing field defaults to the struct's negative"
              " \"not measured\" sentinel, which must not render as zero");
    }

    // ---- LinkStats::alive: this snapshot's bytes against the last one's ----
    {
        AnanBackend backend;
        AnanLinkStatsTestAccess::setConnected(backend, true);

        AnanLinkStatsTestAccess::snapshot(backend, 1000);
        check(backend.linkStats().reported && backend.linkStats().alive,
              "a session's first snapshot compares against zero, so a link that"
              " carried bytes reads alive on its first tick");

        AnanLinkStatsTestAccess::snapshot(backend, 1000);
        check(!backend.linkStats().alive, "a second with no new bytes reads dead");

        AnanLinkStatsTestAccess::snapshot(backend, 5000);
        check(backend.linkStats().alive, "bytes resuming read alive again");

        // A restart that skipped disconnectRadio(), or a dead session's snapshot
        // delivered after its reset: the new session's total is below the cache.
        AnanLinkStatsTestAccess::snapshot(backend, 300);
        check(backend.linkStats().alive && backend.linkStats().rxBytes == 300,
              "a backward jump is a new session, compared against zero and not"
              " against the dead session's total");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
