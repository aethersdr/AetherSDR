// Regression for #7063: a meter the BACKEND deliberately suppresses must be
// reported INCONCLUSIVE naming its gate, never "DEFINED BUT NEVER FED".
//
// The bug: radiocert's keyed-RF precondition uses MeterModel's CONSUMER floor
// (kMinForwardWattsForSwr, 1.1 mW) to decide a run radiated, while a producer
// may gate a derived meter far higher before publishing it at all (Hl2Backend:
// 320 forward counts, ~74 mW). Between the two, rfConfirmed is true, the
// INCONCLUSIVE path is skipped, and a correctly working TX:SWR is condemned
// with a recommendation to delete it. Observed on real HL2 hardware into a
// VNA-measured 2:1 load: forward counts peaked at 76 against a gate of 320.

#include "core/RadioCertification.h"
#include "core/AudioEngine.h"
#include "core/ClientTxTestTone.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

#include <cmath>
#include <cstdio>
#include <memory>
#include <tuple>
#include <utility>

namespace AetherSDR {

// Reaches the private stage and its accumulated state. The stage is driven
// directly rather than through a full run(): this test is about one
// classification decision, and a full run would need a keyed transmitter.
class RadioCertificationTestAccess {
public:
    static QJsonArray stages(const RadioCertification& cert) { return cert.m_stages; }
    static void meterInventory(RadioCertification& cert) { cert.stageMeterInventory(); }
    // The PLUMBING: reads the two keys off the backend health snapshot.
    static void observeKeyedRf(RadioCertification& cert) { cert.observeKeyedRf(); }
    // Stand in for what observeKeyedRf() accumulates across keyed windows, so
    // the test states the radiated/gated condition directly instead of
    // simulating a transmitter.
    static void seedKeyedRf(RadioCertification& cert, int samples, double wattsMax,
                            int gateCounts, int gateClears, int fwdCountsMax,
                            int drivePercent = 10)
    {
        cert.m_keyedWindows = 1;
        cert.m_keyedRfSamples = samples;
        cert.m_keyedFwdWattsMax = wattsMax;
        cert.m_swrGateCounts = gateCounts;
        cert.m_swrGateClears = gateClears;
        cert.m_keyedFwdCountsMax = fwdCountsMax;
        cert.m_keyedDrivePercentMax = drivePercent;
        cert.m_fwdPowerMeterDefined = true;
    }
    // The rf stage's SWR decision, and the live gate read it rests on.
    static std::pair<QString, QString> classifySwrAtKey(bool haveSwr, int gate, int clears,
                                                        int peak, int drive, double tone)
    {
        RadioCertification::SwrGateReading g;
        g.gateCounts = gate;
        g.clearsThisOver = clears;
        g.peakCounts = peak;
        const auto r = RadioCertification::classifySwrAtKey(haveSwr, g, drive, tone);
        return {r.result, r.concern};
    }
    static std::tuple<int, int, int> readSwrGate(const RadioCertification& cert)
    {
        const auto g = cert.readSwrGate();
        return {g.gateCounts, g.clearsThisOver, g.peakCounts};
    }
    static void stageRf(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageRf(o);
    }
    static int swrGateCounts(const RadioCertification& c) { return c.m_swrGateCounts; }
    static int swrGateClears(const RadioCertification& c) { return c.m_swrGateClears; }
    static int keyedFwdCountsMax(const RadioCertification& c) { return c.m_keyedFwdCountsMax; }
};

}  // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    failures += !condition;
}

// Subclasses the REAL Hl2Backend and overrides only the health snapshot, so
// the gate key under test is the one the production backend publishes and the
// family is genuinely "hl2". Never opens a transport or talks to a radio.
class GatedBackend final : public hl2::Hl2Backend {
public:
    HealthSnapshot healthSnapshot() const override { return m_health; }
    bool isConnected() const override { return true; }
    HealthSnapshot m_health;
};

// Define every meter kMeterTable expects, and feed the ones a keyed run feeds.
// TX:SWR is defined but never fed — exactly the state the HL2 leaves it in
// when the forward reading sits below its gate.
void defineAndFeedMeters(MeterModel& meters)
{
    auto def = [&](int idx, const char* src, const char* name, const char* unit) {
        MeterDef d;
        d.index = idx;
        d.source = QString::fromLatin1(src);
        d.name = QString::fromLatin1(name);
        d.unit = QString::fromLatin1(unit);
        d.low = -200.0;
        d.high = 200.0;
        meters.defineMeter(d);
    };
    def(0, "SLC", "LEVEL",    "dBm");
    def(1, "TX",  "MICPEAK",  "dBFS");
    def(2, "TX",  "SWR",      "SWR");
    def(3, "TX",  "FWDPWR",   "dBm");
    def(4, "TX",  "REFPWR",   "dBm");
    def(5, "TX",  "ALC",      "dBFS");
    def(6, "TX",  "ALCGAIN",  "dB");
    def(7, "TX",  "COMPPEAK", "dB");
    def(8, "RAD", "PATEMP",   "degC");

    // Everything except TX:SWR receives a value, which is what the radio does
    // in the band below the gate: the inputs publish, the ratio does not.
    meters.updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"), -120.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("MICPEAK"), -25.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("FWDPWR"), 8.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("REFPWR"), -2.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("ALC"), -12.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("ALCGAIN"), 3.0f);
    meters.updateValueByName(QStringLiteral("TX"),  QStringLiteral("COMPPEAK"), 0.0f);
    meters.updateValueByName(QStringLiteral("RAD"), QStringLiteral("PATEMP"), 34.0f);
}

QJsonObject inventoryOf(const RadioCertification& cert)
{
    const QJsonArray stages = RadioCertificationTestAccess::stages(cert);
    for (const QJsonValue& v : stages) {
        const QJsonObject s = v.toObject();
        if (s.value(QStringLiteral("id")).toString() == QLatin1String("meter-inventory"))
            return s;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv)
{
    // The real Hl2Backend constructor reaches AutomationBridgeSettings::txAllowed()
    // -> AppSettings::instance(), which would open the developer's or CI user's
    // own settings store. Isolate it, as every sibling test that constructs an
    // Hl2Backend does.
    TestSettingsProfile profile(QStringLiteral("radiocert-gated-meter-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    // ---- below the gate: a run that radiated, a meter the backend withheld ----
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 0);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 76);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());

        RadioCertification cert(&radio, nullptr);
        // Radiated by the consumer floor (1.1 mW), no window clearing the
        // producer's gate: the exact band the bug lives in. 76 counts is the
        // hardware reading, at the 10 percent drive the bench run used.
        RadioCertificationTestAccess::seedKeyedRf(cert, 3, 0.0075, 320, 0, 76, 10);
        RadioCertificationTestAccess::meterInventory(cert);

        const QJsonObject stage = inventoryOf(cert);
        const QJsonObject measured = stage.value(QStringLiteral("measured")).toObject();
        const QJsonObject swr = measured.value(QStringLiteral("TX:SWR")).toObject();
        const QString concern = stage.value(QStringLiteral("concern")).toString();

        check(!stage.isEmpty(), "meter-inventory stage was recorded");
        check(swr.value(QStringLiteral("defined")).toBool(),
              "TX:SWR is defined");
        check(!swr.value(QStringLiteral("everFed")).toBool(),
              "TX:SWR was never fed, as the radio leaves it below the gate");

        // THE REGRESSION. Before #7063 this read DEFINED BUT NEVER FED and the
        // concern recommended deleting the meter.
        check(swr.value(QStringLiteral("verdict")).toString() == QLatin1String("INCONCLUSIVE"),
              "a gated TX:SWR is INCONCLUSIVE, not a negative verdict");
        check(!concern.contains(QLatin1String("DEFINED BUT NEVER FED"))
                  || !concern.contains(QLatin1String("TX:SWR")),
              "TX:SWR is not listed under DEFINED BUT NEVER FED");
        check(!concern.contains(QLatin1String("stop defining them"))
                  || !concern.contains(QLatin1String("TX:SWR")),
              "the report does not recommend deleting a working meter");

        // WHAT THE CONCERN SAYS, not only what it omits. The lead and the
        // reason are the first thing an operator reads, and this branch fires
        // only when the run DID radiate — so a lead asserting the opposite, or
        // a reason blaming a slider at 0, contradicts the same report's own
        // keyedRf block. Asserting absence alone let that through (#7063).
        check(!concern.contains(QLatin1String("produced no confirmed RF")),
              "the lead does not claim no RF on a run that radiated");
        check(!concern.contains(QLatin1String("RF power slider")),
              "the reason does not blame the drive slider on a run that radiated");
        check(concern.contains(QLatin1String("this run DID produce RF")),
              "the reason says plainly that the run radiated");

        // The gate must NAME itself — CERTIFICATION.md 2.3.
        const QString reason = swr.value(QStringLiteral("inconclusiveReason")).toString();
        check(reason.contains(QLatin1String("320")),
              "the reason names the gate the backend applies");
        check(reason.contains(QLatin1String("76")),
              "the reason names how far below the gate this run sat");
        // The REASON must rest on the windows, not on the peak: "no keyed
        // window cleared that gate" is the exact statement, where "the peak
        // was below the gate" would be an inference from a quantity the gate
        // is not applied to.
        check(reason.contains(QLatin1String("no keyed window")),
              "the reason states that no window cleared the gate");
        // Below full drive, raising it is the actionable remedy, and the
        // report says where the run keyed so the operator knows by how much.
        check(reason.contains(QLatin1String("raise drive")),
              "below full drive, the remedy is to raise drive");
        check(reason.contains(QLatin1String("10%")),
              "and the remedy says what drive the run used");

        const QJsonObject keyed = measured.value(QStringLiteral("keyedRf")).toObject();
        check(keyed.value(QStringLiteral("backendSwrGateCounts")).toInt() == 320,
              "the report carries the backend's declared gate");
        check(keyed.value(QStringLiteral("backendSwrGateClearsWhileKeyed")).toInt() == 0,
              "the report carries the count of windows that cleared the gate");
        check(keyed.value(QStringLiteral("maxForwardCountsWhileKeyed")).toInt() == 76,
              "the report carries the peak forward counts seen while keyed");
        check(keyed.value(QStringLiteral("maxDrivePercentWhileKeyed")).toInt() == 10,
              "the report carries the drive the run keyed at");
    }

    // ---- Peak above the floor, no window passed the producer's gates: the
    // HL2's key-on and speech-pause gate withheld every window (peak 900,
    // floor 320, zero passes). Judging on the peak would call a working meter
    // a defect, so this must read INCONCLUSIVE, and the reason must name the
    // gate that held rather than advise more drive.
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 0);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 900);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());

        RadioCertification cert(&radio, nullptr);
        RadioCertificationTestAccess::seedKeyedRf(cert, 3, 2.0, 320, 0, 900, 10);
        RadioCertificationTestAccess::meterInventory(cert);

        const QJsonObject stage = inventoryOf(cert);
        const QJsonObject measured = stage.value(QStringLiteral("measured")).toObject();
        const QJsonObject swr = measured.value(QStringLiteral("TX:SWR")).toObject();
        const QString concern = stage.value(QStringLiteral("concern")).toString();

        check(swr.value(QStringLiteral("verdict")).toString() == QLatin1String("INCONCLUSIVE"),
              "an above-gate PEAK with zero clears is INCONCLUSIVE, not a defect");
        check(!concern.contains(QLatin1String("stop defining them"))
                  || !concern.contains(QLatin1String("TX:SWR")),
              "and the report does not recommend deleting the meter");
        const QString reason = swr.value(QStringLiteral("inconclusiveReason")).toString();
        check(reason.contains(QLatin1String("key-on and speech-pause gate"))
                  && reason.contains(QLatin1String("900")),
              "a peak above the floor names the key-on and speech-pause gate");
        check(!reason.contains(QLatin1String("raise drive")),
              "a peak above the floor does not advise raising drive");
    }

    // ---- NIT 4: at full drive, "raise drive" is not a remedy. A forward
    // reading that cannot clear the gate with the slider at maximum is a
    // detector or scaling fault (the #5369 class), and before this change the
    // run at least produced a negative finding. The verdict stays INCONCLUSIVE
    // -- the meter really is suppressed -- but the remedy must point at the
    // forward chain instead of asking for drive that does not exist.
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 0);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 40);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());

        RadioCertification cert(&radio, nullptr);
        RadioCertificationTestAccess::seedKeyedRf(cert, 3, 0.0075, 320, 0, 40, 100);
        RadioCertificationTestAccess::meterInventory(cert);

        const QJsonObject measured =
            inventoryOf(cert).value(QStringLiteral("measured")).toObject();
        const QString reason = measured.value(QStringLiteral("TX:SWR")).toObject()
                                   .value(QStringLiteral("inconclusiveReason")).toString();

        check(!reason.contains(QLatin1String("raise drive")),
              "at full drive the remedy does not ask for more drive");
        check(reason.contains(QLatin1String("100%")),
              "the reason states that drive was already at maximum");
        check(reason.contains(QLatin1String("forward detector")),
              "and it points at the forward detector instead");
    }

    // ---- the gate WAS cleared: silence here IS a defect and must still be
    // reported. This is the other half of what makes the clears count exact,
    // and the mutation that matters most -- a fix that excused every silent
    // gated meter would pass every check above and destroy the diagnostic.
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        // 7 windows cleared the gate and SWR was still never fed.
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 7);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 900);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());

        RadioCertification cert(&radio, nullptr);
        RadioCertificationTestAccess::seedKeyedRf(cert, 3, 2.0, 320, 7, 900, 10);
        RadioCertificationTestAccess::meterInventory(cert);

        const QJsonObject stage = inventoryOf(cert);
        const QJsonObject measured = stage.value(QStringLiteral("measured")).toObject();
        const QJsonObject swr = measured.value(QStringLiteral("TX:SWR")).toObject();
        const QString concern = stage.value(QStringLiteral("concern")).toString();

        // The fix must not silence a REAL defect. Cleared the gate and still
        // nothing: that is the finding the original code existed to make.
        check(swr.value(QStringLiteral("verdict")).toString() != QLatin1String("INCONCLUSIVE"),
              "a meter silent after its gate CLEARED is not excused as inconclusive");
        check(concern.contains(QLatin1String("DEFINED BUT NEVER FED")),
              "a meter silent after its gate cleared is still reported as never fed");
        check(concern.contains(QLatin1String("TX:SWR")),
              "and it is named");
    }

    // ---- a backend declaring no gate is unchanged ----
    {
        RadioModel radio;
        // publishes neither key
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());

        RadioCertification cert(&radio, nullptr);
        RadioCertificationTestAccess::seedKeyedRf(cert, 3, 2.0, -1, -1, -1, 10);
        RadioCertificationTestAccess::meterInventory(cert);

        const QJsonObject stage = inventoryOf(cert);
        const QString concern = stage.value(QStringLiteral("concern")).toString();
        check(concern.contains(QLatin1String("DEFINED BUT NEVER FED")),
              "a backend declaring no gate behaves exactly as before");
    }

    // ---- the PLUMBING: observeKeyedRf() really reads these keys ----
    //
    // Every case above seeds m_swrGateCounts / m_keyedFwdCountsMax directly, so
    // none of them executes the health-snapshot lookup that joins backend to
    // diagnostic. This case drives observeKeyedRf() against a snapshot and
    // checks the two members it is supposed to populate — so a lookup renamed,
    // removed, or pointed at the wrong key fails here rather than silently
    // leaving the gate branch unreachable on real hardware.
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 4);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 212);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        defineAndFeedMeters(radio.meterModel());
        // observeKeyedRf() reaches the snapshot read only after its own
        // preconditions: a defined TX:FWDPWR with a fresh value, which
        // defineAndFeedMeters() has just supplied.
        RadioCertification cert(&radio, nullptr);
        RadioCertificationTestAccess::observeKeyedRf(cert);

        check(RadioCertificationTestAccess::swrGateCounts(cert) == 320,
              "observeKeyedRf reads the gate from the health snapshot");
        check(RadioCertificationTestAccess::swrGateClears(cert) == 4,
              "observeKeyedRf reads the clears count from the health snapshot");
        check(RadioCertificationTestAccess::keyedFwdCountsMax(cert) == 212,
              "observeKeyedRf reads the window peak from the health snapshot");
    }

    // ---- the producer and the consumer cannot name different keys ----
    //
    // Both Hl2Backend::put() and RadioCertification's lookup reference
    // IRadioBackend::HealthSnapshot::kSwrGateCounts / kForwardWindowPeakRaw, so
    // there is no second spelling to get wrong. What remains worth pinning is
    // that the two constants are distinct and non-empty — a rename that made
    // them equal would silently collapse gate and reading onto one map entry.
    {
        const QString gateKey =
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts);
        const QString clearsKey =
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed);
        const QString peakKey =
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw);
        check(!gateKey.isEmpty() && !clearsKey.isEmpty() && !peakKey.isEmpty(),
              "all three diagnostic health keys are named");
        check(gateKey != peakKey && gateKey != clearsKey && clearsKey != peakKey,
              "the gate, its clears count and the reading have distinct keys");
    }

    // ---- the rf stage names the backend's SWR gate (radiocert on the HL2) ----
    //
    // On hardware at 40 % drive, radiocert's -20 dBFS tone peaked at 113
    // forward counts against the HL2's 320-count SWR gate, and the rf stage
    // said "no SWR reading" — sending the reader after a meter and a PA that
    // were both fine. The stage must say the gate was not reached, with the
    // numbers, and must not claim anything when the gate WAS cleared.
    {
        const auto [result, concern] = RadioCertificationTestAccess::classifySwrAtKey(
            false, 320, 0, 113, 40, -20.0);
        check(result == QLatin1String("gate-not-reached"),
              "rf: below the backend's gate is its own result");
        check(concern.contains(QLatin1String("SWR GATE NOT REACHED AT THIS DRIVE")),
              "rf: the concern names the gate, not a missing reading");
        check(!concern.contains(QLatin1String("no SWR reading")),
              "rf: and does not say 'no SWR reading'");
        check(concern.contains(QLatin1String("320")) && concern.contains(QLatin1String("113"))
                  && concern.contains(QLatin1String("40%"))
                  && concern.contains(QLatin1String("-20.0 dBFS")),
              "rf: the concern carries the gate, the peak, the drive and the tone");
        check(concern.contains(QLatin1String("raises neither by itself")),
              "rf: the remedy is stated, never applied");
    }
    {
        const auto [result, concern] = RadioCertificationTestAccess::classifySwrAtKey(
            false, 320, 0, 40, 100, -20.0);
        check(result == QLatin1String("gate-not-reached")
                  && concern.contains(QLatin1String("forward detector"))
                  && !concern.contains(QLatin1String("raise the stimulus")),
              "rf: at full drive the gate points at the forward detector");
    }
    {
        const auto [result, concern] = RadioCertificationTestAccess::classifySwrAtKey(
            false, 320, 0, 900, 40, -20.0);
        check(result == QLatin1String("gate-withheld")
                  && concern.contains(QLatin1String("key-on and speech-pause gate")),
              "rf: a peak above the gate with no clears names the key-on gate");
    }
    {
        // Cleared and still silent: the one case the gate cannot excuse.
        const auto [result, concern] = RadioCertificationTestAccess::classifySwrAtKey(
            false, 320, 7, 900, 40, -20.0);
        check(result == QLatin1String("no-reading")
                  && concern.contains(QLatin1String("no SWR reading"))
                  && concern.contains(QLatin1String("SWR was due")),
              "rf: a silent SWR after the gate cleared stays a finding");
    }
    {
        const auto [result, concern] = RadioCertificationTestAccess::classifySwrAtKey(
            false, -1, -1, -1, 40, -20.0);
        check(result == QLatin1String("no-reading")
                  && concern.contains(QLatin1String("no SWR reading")),
              "rf: a backend declaring no gate reads as before");
        const auto [ok, none] = RadioCertificationTestAccess::classifySwrAtKey(
            true, 320, 0, 113, 40, -20.0);
        check(ok == QLatin1String("reading") && none.isEmpty(),
              "rf: an SWR reading is a reading, whatever the gate says");
    }
    {
        // The plumbing: the live gate read the rf stage takes inside its key.
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        auto* owned_ = static_cast<GatedBackend*>(owned.get());
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateCounts), 320);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kSwrGateClearsWhileKeyed), 0);
        owned_->m_health.values.insert(
            QLatin1StringView(IRadioBackend::HealthSnapshot::kForwardWindowPeakRaw), 113);
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        RadioCertification cert(&radio, nullptr);
        const auto [gate, clears, peak] = RadioCertificationTestAccess::readSwrGate(cert);
        check(gate == 320 && clears == 0 && peak == 113,
              "rf: readSwrGate reads all three keys off the health snapshot");
    }

    // ---- the rf stimulus is the bridge option, and its default is unchanged ----
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        AudioEngine audio;   // constructed, never started: no device, no socket
        check(audio.clientTxTestTone() != nullptr, "the engine owns a test tone");

        RadioCertification::Options defaults;
        check(defaults.rfStimulusDbfs == -20.0, "the default rf stimulus stays -20 dBFS");

        // No TxController: the key is refused before admission, so nothing can
        // transmit, and the stage still sets the tone it would have keyed with.
        RadioCertification cert(&radio, &audio);
        RadioCertification::Options o;
        o.settleMs = 0;
        o.rfStimulusDbfs = -6.0;
        RadioCertificationTestAccess::stageRf(cert, o);
        check(std::fabs(audio.clientTxTestTone()->levelDb() - (-6.0f)) < 0.01f,
              "rf: the stage keys with the requested tone level");
        check(!audio.clientTxTestTone()->isEnabled(), "rf: and switches the tone off after");
        QJsonObject rf;
        for (const QJsonValue& v : RadioCertificationTestAccess::stages(cert)) {
            if (v.toObject().value(QStringLiteral("id")).toString() == QLatin1String("rf"))
                rf = v.toObject();
        }
        const QJsonObject m = rf.value(QStringLiteral("measured")).toObject();
        check(m.value(QStringLiteral("stimulusToneDbfs")).toDouble() == -6.0,
              "rf: the report says which tone it keyed with");
        check(m.value(QStringLiteral("drivePercentWhileKeyed")).isNull(),
              "rf: a refused key reports no keyed drive");
    }

    // ---- the report states the power ceiling the run applied ----
    //
    // Bridge runs set Options::maxRfPowerPercent and the clamp reads it, but
    // the report read a second field nothing set, so every hardware run said
    // txPowerCeilingPercent -1 while the ceiling was live and clamping.
    {
        RadioModel radio;
        std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new GatedBackend()));
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        radio.transmitModel().setRfPower(80);
        check(radio.transmitModel().rfPower() == 80, "operator RF power is 80");

        RadioCertification cert(&radio, nullptr);
        RadioCertification::Options o;
        o.phase = RadioCertification::Phase::Tune;   // keys nothing
        o.maxRfPowerPercent = 40;
        const QJsonObject report = cert.run(o);
        const QJsonObject r = report.value(QStringLiteral("radio")).toObject();
        check(r.value(QStringLiteral("txPowerCeilingPercent")).toInt(-99) == 40,
              "the report carries the ceiling the run applied");
        check(r.value(QStringLiteral("rfPowerClampedFromPercent")).toInt(-99) == 80,
              "and the operator setting it clamped");
        check(r.value(QStringLiteral("rfStimulusDbfs")).toDouble() == -20.0,
              "and the rf stimulus level");
        check(radio.transmitModel().rfPower() == 80,
              "the operator's RF power is restored after the run");

        RadioCertification cert2(&radio, nullptr);
        radio.transmitModel().setRfPower(25);
        const QJsonObject r2 = cert2.run(o).value(QStringLiteral("radio")).toObject();
        check(r2.value(QStringLiteral("txPowerCeilingPercent")).toInt(-99) == 40
                  && r2.value(QStringLiteral("rfPowerClampedFromPercent")).isNull(),
              "below the ceiling: the ceiling is reported and nothing was clamped");

        RadioCertification cert3(&radio, nullptr);
        RadioCertification::Options none;
        none.phase = RadioCertification::Phase::Tune;
        const QJsonObject r3 = cert3.run(none).value(QStringLiteral("radio")).toObject();
        check(r3.value(QStringLiteral("txPowerCeilingPercent")).toInt(-99) == -1,
              "no ceiling reads -1");
    }

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
