#include "core/RadioCertification.h"
#include "core/backends/AutoRfGainControl.h"

#include "core/RadioCertificationMath.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientQuindarTone.h"
#include "core/LogManager.h"
#include "core/ClientTxTestTone.h"
#include "core/MeterSurfaces.h"
#include "core/PcmFrame.h"
#include "models/MeterModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QByteArray>
#include <QDateTime>
#include <QEventLoop>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <optional>
#include <vector>

namespace AetherSDR {

using certmath::db;
using certmath::rms;
using certmath::tonePower;


namespace {

// The meter table from docs/radio-certification.md, as data so docs and tool
// can't drift; unpublished rows show as "defined but never fed" or absent.
// No unit column: expected units live only in kMeterSurfaces (acceptedUnits),
// joined by key. A row with no surface entry gets no unit verdict.
struct MeterSpec {
    const char* source;
    const char* name;
    bool expectedOnHl2;      // physically producible by this radio class

    // Cannot carry a value unless the transmitter was keyed at all.
    //
    // TX:MICPEAK, TX:ALC and TX:COMPPEAK are computed host-side from the audio
    // heading for the modulator, so a key with the drive slider at zero feeds
    // all three — but a key the radio REFUSED feeds none of them, and the run
    // that exposed all of this had three refusals and reported the resulting
    // silence as three meters that are never fed.
    bool needsKey;

    // Cannot carry a value unless the PA actually produced forward power.
    //
    // A strictly stronger condition than needsKey, and the distinction is the
    // point: TX:SWR, TX:FWDPWR and TX:REFPWR measure RF that did not happen
    // even when the key went down cleanly. Reporting them as "defined but never
    // fed" after a silent key is the false finding CERTIFICATION.md 1.37
    // records — and its own concern text recommends deleting the meter.
    bool needsForwardPower;

    const char* note;
};

// `note` is factual as of 2026-08-10, checked against Hl2Backend rather than
// remembered. Four rows previously described meters as unpublished or unwired
// that are both defined and fed, and were marked expectedOnHl2 = false while
// being present — which switched off the one check (`expectedButMissing`) that
// could notice them regressing.
//                              hl2   key    rf
constexpr MeterSpec kMeterTable[] = {
    {"SLC", "LEVEL",    true,  false, false, "receive signal level"},
    {"TX",  "MICPEAK",  true,  true,  false, "pre-ALC microphone peak; host-side, so a "
                                             "zero-drive key still feeds it"},
    {"TX",  "SWR",      true,  true,  true,  "ratio - meaningful uncalibrated, but only "
                                             "published above the forward-power floor"},
    {"TX",  "FWDPWR",   true,  true,  true,  "published in dBm as wattsToDbm(directional"
                                             "Watts(raw)) through the peak hold; the "
                                             "reference curve is uncalibrated, the meter "
                                             "is not absent"},
    {"TX",  "REFPWR",   true,  true,  true,  "published in dBm as wattsToDbm(directional"
                                             "Watts(raw)); same uncalibrated curve as "
                                             "FWDPWR"},
    {"TX",  "ALC",      true,  true,  false, "host ALC; MeterModel::swAlc() consumes it "
                                             "and the Phone/CW ALC gauges render it"},
    // Host ALC gain, distinct from TX:ALC (post-ALC peak, which sits near target):
    // this moves with how hard the stage works. Host-side, so a zero-drive key
    // feeds it (needsForwardPower false). Every kMeterSurfaces entry needs a row
    // here or its unit verdict is lost (CERTIFICATION.md 1.38);
    // tests/meter_surfaces_test.cpp checks the join but is not in the PR CI gate
    // (.github/ci-test-gate.txt) — it runs on main pushes and weekly sanitizers.
    {"TX",  "ALCGAIN",  true,  true,  false, "gain the host ALC is applying, in dB; "
                                             "MeterModel::alcGainDb() consumes it and "
                                             "no GUI surface renders it yet (#5636)"},
    {"TX",  "COMPPEAK", true,  true,  false, "host speech processor, polled onto the "
                                             "meter at 20 Hz; reads 0 with PROC off, "
                                             "which is a value and not a silence"},
    {"RAD", "PATEMP",   true,  false, false, "rise under key is the check, not the value"},
    {"RAD", "+13.8A",   false, false, false, "no supply telemetry on this radio"},
};

}  // namespace

RadioCertification::RadioCertification(RadioModel* radio, AudioEngine* audio,
                                       std::shared_ptr<TxController> controller)
    : m_radio(radio), m_audio(audio), m_txController(std::move(controller)) {}

void RadioCertification::spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void RadioCertification::record(const QString& id, const QString& title,
                             const QJsonObject& measured,
                             const QString& observation,
                             const QString& concern,
                             const QString& reference,
                             bool meterDependent)
{
    QJsonObject stage{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("measured"), measured},
        {QStringLiteral("observation"), observation},
    };
    if (meterDependent)
        stage[QStringLiteral("meterDependent")] = true;
    if (!concern.isEmpty())
        stage[QStringLiteral("concern")] = concern;
    if (!reference.isEmpty())
        stage[QStringLiteral("reference")] = reference;
    m_stages.append(stage);
}

void RadioCertification::setKeyObserver(KeyObserver observer)
{
    m_onKey = std::move(observer);
}

bool RadioCertification::keyedNow() const
{
    if (!m_radio)
        return false;
    const auto& tx = m_radio->transmitModel();
    return tx.isTransmitting() || tx.isMox() || tx.isTuning();
}

bool RadioCertification::keyViaOperatorPath(bool on)
{
    if (!m_radio)
        return false;
    const TxCoordinator::Operation previous = m_radio->transmitOperation();
    const bool keyedBefore = keyedNow();

    if (on) {
        // The authorization controller is captured once for the diagnostic,
        // never fetched anew after one of its nested event-loop waits.
        if (!m_txController || !m_txController->valid()
            || !m_txController->belongsTo(m_radio)) {
            ++m_keyRefusals;
            return false;
        }
        m_keyInput = m_txController->capture(TxController::Activity::Mox);
        if (!m_keyInput.start()) {
            ++m_keyRefusals;
            return false;
        }
        if (m_onKey) {
            m_onKey(true, previous, keyedBefore);
        }

        // CONFIRM THE KEY REACHED THE RADIO. requestPttOn returns void and
        // silently does nothing when runPttPreflight() refuses — a band-limit
        // block, a missing antenna, an interlock. Every stage below then
        // measures an unkeyed radio and reports its silence as a defect in
        // whatever it happens to be testing: "audio never reached the
        // modulator", "the transmitter is not producing RF". The diagnostic
        // would blame the chain for a refusal it never noticed.
        spin(250);
        if (!m_keyInput.valid() || !keyedNow()) {
            ++m_keyRefusals;
            if (m_onKey)
                m_onKey(false, previous, keyedBefore);
            return false;
        }
        return true;
    }

    m_keyInput.stop();

    // WAIT FOR THE RADIO TO ACTUALLY UNKEY BEFORE DISARMING THE WATCHDOG.
    //
    // With Quindar enabled in a phone mode, requestPttOff does NOT unkey: it
    // starts an outro tone and defers the real dispatchMoxOff() behind a
    // single-shot QTimer for the outro duration. Firing m_onKey(false)
    // immediately therefore disarmed the force-unkey watchdog while the radio
    // was still transmitting — the backstop switched off during the one window
    // it exists to cover.
    for (int waited = 0; waited < 2500 && keyedNow(); waited += 100)
        spin(100);

    if (m_onKey)
        m_onKey(false, previous, keyedBefore);
    return !keyedNow();
}

QJsonObject RadioCertification::meterSnapshot() const
{
    QJsonObject out;
    if (!m_radio)
        return out;
    const auto& meters = m_radio->meterModel();
    // FRESHNESS MATTERS MORE THAN VALUE. MeterModel keeps last-known readings
    // and never clears them, so "is there forward power now" answered from a
    // bare value() is really "was there ever". That produced a false carrier
    // report: the SWR left over from the previous keyed stage was read as
    // evidence of a carrier while the transmitter was sending silence.
    //
    // Anything older than this is reported but marked stale, and the stages that
    // draw conclusions ignore it.
    constexpr qint64 kFreshMs = 3000;
    auto put = [&](const char* key, const char* source, const char* name) {
        const int idx = meters.findMeter(QString::fromLatin1(source),
                                         QString::fromLatin1(name));
        if (idx < 0)
            return;
        const qint64 age = meters.valueAgeMs(idx);
        if (age < 0)
            return;
        out[QString::fromLatin1(key)] = static_cast<double>(meters.value(idx));
        out[QString::fromLatin1(key) + QStringLiteral("AgeMs")] = static_cast<double>(age);
        if (age > kFreshMs)
            out[QString::fromLatin1(key) + QStringLiteral("Stale")] = true;
    };
    put("micPeakDbfs", "TX", "MICPEAK");
    put("swr", "TX", "SWR");
    put("paTempC", "RAD", "PATEMP");
    put("sLevelDbm", "SLC", "LEVEL");
    return out;
}

QJsonObject RadioCertification::renderedSnapshot() const
{
    QJsonObject out;
    if (!m_radio)
        return out;
    const auto& meters = m_radio->meterModel();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto ageOf = [now](qint64 stamp) { return stamp > 0 ? now - stamp : qint64(-1); };

    // FORWARD POWER, gated the way RigctlProtocol gates RFPOWER_METER_WATTS:
    // suppress a cached last-transmit value rather than presenting it as
    // current. The value reported is fwdPowerInstant(), which is what the TX
    // Controls power gauge binds to, so this is the gauge's number and not the
    // seam's dBm.
    const qint64 fwdAge = ageOf(meters.fwdPowerUpdatedAtMs());
    const bool fwdLive = fwdAge >= 0 && fwdAge <= MeterModel::kTxMeterStaleMs;
    out[QStringLiteral("fwdPowerWatts")] =
        fwdLive ? QJsonValue(static_cast<double>(meters.fwdPowerInstant())) : QJsonValue();
    out[QStringLiteral("fwdPowerAgeMs")] = static_cast<double>(fwdAge);
    out[QStringLiteral("fwdPowerLive")] = fwdLive;

    // SWR through swrIfLive(), which is THE liveness predicate — the same one
    // behind the signals' swrValid flag, both snapshot arrays and the bridge
    // scalar. Reading swr() raw returns m_swr's 1.0f initialiser, so a meter
    // that has never been fed and a perfect match are indistinguishable, and
    // this probe reported "1.0" in the same object that reported "never fed".
    const std::optional<float> liveSwr = meters.swrIfLive();
    out[QStringLiteral("swr")] =
        liveSwr ? QJsonValue(static_cast<double>(*liveSwr)) : QJsonValue();
    out[QStringLiteral("swrAgeMs")] = static_cast<double>(ageOf(meters.swrUpdatedAtMs()));
    out[QStringLiteral("swrLive")] = liveSwr.has_value();

    // ALC has no liveness predicate on the model — the Phone/CW gauges are
    // signal-driven and simply keep displaying the last value they were sent.
    // That is exactly §1.11's stale reading, so age it from the meter's own
    // timestamp here rather than reporting a bare float. Reported unaged, an
    // ALC value left over from the previous key reads as a live one.
    const int alcIdx = meters.findMeter(QStringLiteral("TX"), QStringLiteral("ALC"));
    const qint64 alcAge = alcIdx >= 0 ? meters.valueAgeMs(alcIdx) : -1;
    const bool alcLive = alcAge >= 0 && alcAge <= MeterModel::kTxMeterStaleMs;
    out[QStringLiteral("alcDbfs")] =
        alcLive ? QJsonValue(static_cast<double>(meters.swAlc())) : QJsonValue();
    out[QStringLiteral("alcValue")] =
        alcLive ? QJsonValue(static_cast<double>(meters.alcValue())) : QJsonValue();
    out[QStringLiteral("alcUnit")] = meters.alcUnit();
    out[QStringLiteral("alcAgeMs")] = static_cast<double>(alcAge);
    out[QStringLiteral("alcLive")] = alcLive;
    return out;
}

void RadioCertification::observeKeyedRf()
{
    if (!m_radio)
        return;
    const auto& meters = m_radio->meterModel();
    ++m_keyedWindows;

    // A radio that defines no forward-power meter cannot answer the question,
    // and that is a different state from one that answered zero. Recorded so
    // the verdict can say WHICH.
    if (meters.findMeter(QStringLiteral("TX"), QStringLiteral("FWDPWR")) < 0)
        return;
    m_fwdPowerMeterDefined = true;

    const qint64 stamp = meters.fwdPowerUpdatedAtMs();
    if (stamp <= 0)
        return;
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - stamp;
    if (age > MeterModel::kTxMeterStaleMs)
        return;   // a reading from a previous key proves nothing about this one
    ++m_keyedRfSamples;
    m_keyedFwdWattsMax = std::max(m_keyedFwdWattsMax,
                                  static_cast<double>(meters.fwdPowerInstant()));
}

bool RadioCertification::keyedRfConfirmed() const
{
    return m_keyedRfSamples > 0 && m_keyedFwdWattsMax > kKeyedRfFloorWatts;
}

// ---------------------------------------------------------------------------
// Receive stages. Every one of these is a transcription of docs/HERMES.md 15.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Meter stages. LAST, because nothing above may depend on them.
// ---------------------------------------------------------------------------

void RadioCertification::stageControlEffect(const Options& o)
{
    // CONTROLS ARE CERTIFIED BY THEIR EFFECT, NEVER BY READBACK.
    //
    // A slider that reports the value it was handed proves only that the model
    // has a variable. The mode map passed for twelve modes while the backend
    // mapped nine of them, and RTTY still reads back perfectly while being
    // demodulated as USB. So each control here is moved by a known amount and
    // the CONSEQUENCE is measured.
    if (!m_audio || !m_radio)
        return;

    auto keyedMicPeak = [&](int micGainPercent) -> double {
        m_audio->setPcMicGain(micGainPercent);
        if (auto* t = m_audio->clientTxTestTone()) {
            t->setFrequencyHz(1000.0f); t->setLevelDb(-20.0f); t->setEnabled(true);
        }
        const bool keyed = keyViaOperatorPath(true);
        spin(o.settleMs);
        const QJsonObject s2 = meterSnapshot();
        // Evidence for the keyed-RF precondition, taken while the key is still
        // down. Every keyed window in the run contributes, because one silent
        // key does not prove the PA never enabled and one loud one is enough to
        // prove it did.
        if (keyed)
            observeKeyedRf();
        keyViaOperatorPath(false);
        // A STALE OR REFUSED READING IS NOT A MEASUREMENT. A frozen TX:MICPEAK
        // returns the same number at both gain settings, so the delta is 0 dB
        // and the stage fabricates "the control and the meter disagree about
        // what the gain is" — a confident finding about a control it never
        // observed. Same for a key the radio refused.
        if (auto* t = m_audio->clientTxTestTone()) t->setEnabled(false);
        spin(700);
        if (!keyed || s2.value(QStringLiteral("micPeakDbfsStale")).toBool())
            return -999.0;
        return s2.value(QStringLiteral("micPeakDbfs")).toDouble(-999.0);
    };

    const int restoreMic = AppSettings::instance().value("PcMicGain", 100).toInt();
    const double micFull = keyedMicPeak(100);
    const double micHalf = keyedMicPeak(50);
    m_audio->setPcMicGain(restoreMic);

    // Halving a linear gain is -6.02 dB. This is arithmetic, not a guess about
    // the radio, which is what makes it a usable threshold on hardware nobody
    // has characterised.
    const double micDelta = micFull - micHalf;

    // This stage does not exercise the RF power slider. A halving stimulus is
    // unusable on HL2: the gateware decodes only the drive register's top nibble
    // (44 % and 50 % both map to nibble 7), and HL2FilterE3 is nonlinear, so the
    // ratio doesn't cancel (measured −4.44 dB for 100→50 %, −2.33 dB for 50→25 %).
    // The power row is certified by monotonicity instead (docs/HERMES.md 17.5, 17.7).
    QJsonObject m{
        {QStringLiteral("micGain100Dbfs"), micFull},
        {QStringLiteral("micGain50Dbfs"), micHalf},
        {QStringLiteral("micGainDeltaDb"), micDelta},
        {QStringLiteral("micGainExpectedDb"), -6.02},
        {QStringLiteral("rfPowerExercised"), false},
    };

    QStringList problems;
    if (micFull < -998.0 || micHalf < -998.0)
        problems << QStringLiteral("no mic peak reading — cannot certify mic gain by effect");
    else if (std::fabs(micDelta - 6.02) > 1.5)
        problems << QStringLiteral(
            "halving mic gain moved the transmitted level by %1 dB, not the 6.0 dB "
            "a linear halving must produce — the control and the meter disagree "
            "about what the gain is")
            .arg(micDelta, 0, 'f', 1);

    // RF gain, certified by effect, no family gate. Seam path: rfGainChanged ->
    // RadioModel::setPanRfGainFor -> backend (HL2: MetisClient::setLnaGainDb, AD9866
    // 0x0a[5:0]). Expected S-meter delta is ZERO (Hl2DbReference tracks the gain,
    // docs/HERMES.md 17.4), but a delta can't distinguish a stuck register from
    // converter noise on a quiet band, so it's reported as evidence; the verdict
    // rests on the echo (applyLnaGainDb echoes the applied, clamped value). The
    // effect half needs raw pre-reference dBFS, not on the seam (CERTIFICATION.md 2.4).
    auto settledSLevel = [&]() -> double {
        // LET THE EMA CATCH UP. docs/HERMES.md 17.6: every WDSP sample is smoothed
        // (decay alpha 0.15 at ~47 samples/s, so ~0.7 s to settle) and one is
        // published per 100 ms gate. A reading taken straight after the step is
        // a blend of both gain settings, which halves the delta and lands it
        // exactly between the two hypotheses this check distinguishes.
        spin(1200);
        const QJsonObject s = meterSnapshot();
        if (s.value(QStringLiteral("sLevelDbmStale")).toBool())
            return -999.0;
        return s.value(QStringLiteral("sLevelDbm")).toDouble(-999.0);
    };

    const QString panId = m_radio->panId();
    PanadapterModel* pan = panId.isEmpty() ? nullptr : m_radio->panadapter(panId);
    if (!pan) {
        m[QStringLiteral("rfGainExercised")] = false;
        m[QStringLiteral("rfGainNotExercised")] = QStringLiteral(
            "no active panadapter, so the route the operator's RF Gain slider "
            "actually uses does not exist to be driven");
    } else {
        // Disarm auto RF gain for this ~2.8 s probe: a loop moving the gain would make
        // `echoed != target` (a false "control did not reach the backend") and
        // invalidate startGain. Restored on every exit path. Only re-armed if it was
        // ARMED before; a run must never switch the feature on.
        auto* autoGain = m_radio->autoRfGain();
        const bool autoGainWasOn = autoGain && autoGain->isArmed();
        if (autoGainWasOn) {
            autoGain->setArmed(false);
            spin(200);
        }
        const int startGain = pan->rfGain();
        const int low = pan->rfGainLow();
        const int high = pan->rfGainHigh();
        // 8 dB: large enough to clear S-meter noise, small enough to leave the
        // front end somewhere ordinary. Bounded by the range the BACKEND
        // published rather than a constant pasted from one radio — the HL2
        // reports -12..+48 in 1 dB steps against the model's Flex-shaped default.
        constexpr int kProbeStepDb = 8;
        int target = startGain + kProbeStepDb;
        if (target > high)
            target = startGain - kProbeStepDb;
        target = qBound(low, target, high);
        const int stepDb = target - startGain;

        // AGC-T is referred to the LNA gain by the backend (Hl2DbReference::
        // agcCeilingDb), so a gain change must leave the operator's 0..100 AGC-T
        // untouched. The derived ceiling isn't on the seam; this certifies only that
        // the operator's number did not move. Expected delta zero.
        SliceModel* agcSlice = m_radio->slice(0);
        const int agcTBefore = agcSlice ? agcSlice->agcThreshold() : -1;

        const double before = settledSLevel();
        m_radio->setPanRfGainFor(panId, target);
        const double after = settledSLevel();
        const int echoed = pan->rfGain();
        const int agcTAfter = agcSlice ? agcSlice->agcThreshold() : -1;
        m_radio->setPanRfGainFor(panId, startGain);   // leave it where we found it
        spin(400);
        // Re-arm after the gain is restored. Re-fetch the pointer: it is borrowed per
        // call (AutoRfGainControl.h), and spin() has run event loops since, where a
        // link drop can reset m_backend and leave the old pointer dangling.
        if (autoGainWasOn) {
            if (auto* ag = m_radio->autoRfGain()) {
                ag->setArmed(true);
            }
        }
        m[QStringLiteral("autoRfGainSuspended")] = autoGainWasOn;

        const bool haveLevels = before > -998.0 && after > -998.0;
        const double delta = after - before;
        m[QStringLiteral("rfGainExercised")] = stepDb != 0;
        m[QStringLiteral("rfGainStartDb")] = startGain;
        m[QStringLiteral("rfGainRequestedDb")] = target;
        m[QStringLiteral("rfGainEchoedDb")] = echoed;
        m[QStringLiteral("rfGainReachedBackend")] = echoed == target;
        m[QStringLiteral("rfGainStepDb")] = stepDb;
        m[QStringLiteral("sLevelBeforeDbm")] = before > -998.0 ? QJsonValue(before) : QJsonValue();
        m[QStringLiteral("sLevelAfterDbm")] = after > -998.0 ? QJsonValue(after) : QJsonValue();
        m[QStringLiteral("sLevelDeltaDb")] = haveLevels ? QJsonValue(delta) : QJsonValue();
        // Zero, and see above for why it is not the step size — and for why a
        // reading near sLevelDeltaIfRegisterNotWrittenDb is NOT reported as a
        // defect. Both numbers are published so a reader can place the measured
        // delta between them instead of being handed a verdict the measurement
        // cannot support.
        m[QStringLiteral("sLevelExpectedDeltaDb")] = 0.0;
        m[QStringLiteral("sLevelDeltaIfRegisterNotWrittenDb")] = -stepDb;
        m[QStringLiteral("sLevelDeltaIsConclusive")] = false;
        m[QStringLiteral("sLevelDeltaCaveat")] = QStringLiteral(
            "a delta near %1 dB has two causes that this measurement cannot "
            "separate: the LNA register never took the value, or the reading is "
            "dominated by converter noise that does not rise with the gain. Only "
            "the raw pre-reference dBFS distinguishes them and the seam does not "
            "expose it").arg(-stepDb);
        if (agcSlice) {
            m[QStringLiteral("agcThresholdBefore")] = agcTBefore;
            m[QStringLiteral("agcThresholdAfter")] = agcTAfter;
            m[QStringLiteral("agcThresholdExpectedDelta")] = 0;
        }

        if (stepDb == 0) {
            m[QStringLiteral("rfGainNotExercised")] = QStringLiteral(
                "the published gain range %1..%2 dB leaves no room for a probe "
                "step from %3 dB").arg(low).arg(high).arg(startGain);
        } else if (echoed != target) {
            // THE ONE CONCERN THIS CHECK EARNS. The echo carries the value the
            // hardware clamped to, so its absence means the command did not
            // reach the backend at all — which is the claim the deleted
            // hardcoded assertion used to make without ever testing it.
            problems << QStringLiteral(
                "the RF Gain control did not reach the backend: asked for %1 dB "
                "and the pan reports %2 dB. This route echoes the value the "
                "hardware took, so a missing echo means the command went nowhere")
                .arg(target).arg(echoed);
        } else if (!haveLevels) {
            problems << QStringLiteral(
                "no S-meter reading either side of the RF gain step — the gain "
                "reached the backend but its effect could not be measured");
        }
        // Reported whether or not the levels came back: this one does not
        // depend on a meter, so a quiet band cannot excuse it.
        if (agcSlice && stepDb != 0 && agcTAfter != agcTBefore) {
            problems << QStringLiteral(
                "an RF gain step of %1 dB moved the operator's AGC threshold "
                "from %2 to %3. The gain is compensated below the slider, in "
                "the derived WDSP ceiling — moving the operator's own setpoint "
                "makes it walk every time the gain changes")
                .arg(stepDb).arg(agcTBefore).arg(agcTAfter);
        }
    }

    record(QStringLiteral("control-effect"),
           QStringLiteral("Controls are certified by their effect, not readback"),
           m,
           QStringLiteral(
               "Halving a linear gain is -6.02 dB. That is arithmetic rather than "
               "a property of this radio, which is what makes it a threshold that "
               "transfers to hardware nobody has characterised yet. The RF gain "
               "check is the same idea against a different invariant: the display "
               "reference tracks the commanded gain, so the S-meter must not move "
               "at all, and the amount it moves if the register was never written "
               "is known exactly."),
           problems.join(QStringLiteral("; ")),
           QStringLiteral("docs/radio-certification.md — Controls"));
}

namespace {

// Sampling cadence for the control stages: fine enough to time a confirmation
// read (~60 ms behind the write on Icom), coarse enough to stay cheap.
constexpr int kControlSampleMs = 100;
// Per probed value: the write, its confirmation read, and the model update.
constexpr int kControlProbeMs = 1200;
// The ends of a range hold this long instead: two IC-7300MK2 controls polls
// (3 s each), so a periodic re-read that re-asserts a wrong value is inside the
// window. The on-at-0 squelch defect was re-asserted that way (#6175).
constexpr int kBoundaryHoldMs = 6500;
// Per front-end transition: longer than one 3 s poll, so a coupled control the
// radio changed without reporting converges inside the window and is timed.
constexpr int kInterlockWatchMs = 4000;

QJsonValue msOrNull(int index)
{
    return index < 0 ? QJsonValue() : QJsonValue((index + 1) * kControlSampleMs);
}

}  // namespace

void RadioCertification::stageControlDomain(const Options& o)
{
    // A SCALED CONTROL READS BACK WHERE IT WAS WRITTEN, ACROSS ITS WHOLE RANGE.
    //
    // The IC-7300MK2 wrote PROC as NOR/DX/DX+ and decoded the confirmation read
    // as a percent, so NOR and DX snapped to DX+ (#6171, fixed by #6174). Its
    // squelch, with no enable register, read its own "on at 0" write back as Off
    // and stuck there (#6172, fixed by #6175). Both backends had round-trip
    // tests that shared the wrong convention (§1.1). So each published value is
    // written through the operator's setter and the model is watched until the
    // radio has answered — and, at the ends of the range, through two polls.
    //
    // A model that never reads back also holds. This proves no contradicting
    // readback arrived, not that one did; persist's restart read is the
    // independent half (CERTIFICATION.md 1.41).
    if (!m_radio)
        return;
    const RadioCapabilities caps = m_radio->backendCapabilities();
    QStringList problems;
    QJsonObject m;
    bool interrupted = false;

    // spin() runs the event loop, where a disconnect deletes slices and can
    // delete the radio model; `alive` is re-asked after every wait.
    auto sweep = [&](const QString& name, int maximum,
                     const std::function<bool()>& alive,
                     const std::function<void(int)>& write,
                     const std::function<int()>& read) {
        QJsonArray rows;
        QJsonArray failed;
        const std::vector<int> values = certmath::domainProbeValues(maximum);
        for (int value : values) {
            if (!alive()) {
                interrupted = true;
                break;
            }
            const bool end = value == values.front() || value == values.back();
            write(value);
            std::vector<int> samples;
            for (int t = 0; t < (end ? kBoundaryHoldMs : kControlProbeMs); t += kControlSampleMs) {
                spin(kControlSampleMs);
                if (!alive())
                    break;
                samples.push_back(read());
            }
            if (!alive() || samples.empty()) {
                interrupted = true;
                break;
            }
            const certmath::Readback r = certmath::readbackOf(samples, value);
            const int landed = r.departedAt >= 0 ? samples[static_cast<std::size_t>(r.departedAt)]
                                                 : samples.back();
            rows.append(QJsonObject{
                {QStringLiteral("written"), value},
                {QStringLiteral("heldMs"), end ? kBoundaryHoldMs : kControlProbeMs},
                {QStringLiteral("agreedAtMs"), msOrNull(r.agreedAt)},
                {QStringLiteral("departedAtMs"), msOrNull(r.departedAt)},
                {QStringLiteral("landedAt"), landed},
                {QStringLiteral("held"), r.held()},
            });
            if (!r.held()) {
                failed.append(value);
                problems << (r.agreedAt < 0
                    ? QStringLiteral("%1 %2 was never read back as written: the model "
                                     "sat at %3")
                          .arg(name).arg(value).arg(landed)
                    : QStringLiteral("%1 %2 read back as written, then moved to %3 "
                                     "after %4 ms")
                          .arg(name).arg(value).arg(landed)
                          .arg((r.departedAt + 1) * kControlSampleMs));
            }
        }
        return QJsonObject{
            {QStringLiteral("maximum"), maximum},
            {QStringLiteral("probes"), rows},
            {QStringLiteral("notHeld"), failed},
        };
    };

    // ---- speech processor level: written only while PROC is on (Icom 14 0E) ----
    if (caps.speechProcessorControl || m_radio->usesFlexCommandPlane()) {
        auto& tx = m_radio->transmitModel();
        const bool enableWas = tx.speechProcessorEnable();
        const int levelWas = tx.speechProcessorLevel();
        // Restored on every exit: a run continues into keyed stages, which must
        // not inherit a force-enabled processor at a probe level.
        const auto restore = qScopeGuard([&] {
            if (!m_radio)
                return;
            auto& t = m_radio->transmitModel();
            t.setSpeechProcessorLevel(levelWas);
            spin(400);
            if (!enableWas && m_radio)
                m_radio->transmitModel().setSpeechProcessorEnable(false);
        });
        if (!enableWas) {
            tx.setSpeechProcessorEnable(true);
            spin(400);
        }
        const QPointer<RadioModel> radio(m_radio);
        QJsonObject proc = sweep(caps.speechProcessorControl
                                     ? caps.speechProcessorControl->label
                                     : QStringLiteral("PROC"),
                                 m_radio ? m_radio->transmitModel().speechProcessorLevelMaximum() : 2,
                                 [radio] { return !radio.isNull(); },
                                 [radio](int v) { radio->transmitModel().setSpeechProcessorLevel(v); },
                                 [radio] { return radio->transmitModel().speechProcessorLevel(); });
        proc[QStringLiteral("enabledForSweep")] = !enableWas;
        m[QStringLiteral("speechProcessorLevel")] = proc;
    } else {
        m[QStringLiteral("speechProcessorLevel")] = QStringLiteral(
            "not exercised: no backend applies a speech processor level");
    }

    // ---- squelch: the sample carries the enable, -1 for Off ----
    //
    // A level-only comparison reads "on at 0" back as 0 and certifies the
    // defect. Auto SQL writes the slice on every pan frame, so with it engaged
    // a "moved to N" here would be Auto, not a decode: decline instead.
    const std::optional<bool> autoBefore = o.autoSquelchEngaged ? o.autoSquelchEngaged()
                                                                : std::nullopt;
    const QPointer<SliceModel> slice(m_radio ? m_radio->slice(0) : nullptr);
    if (autoBefore.value_or(false)) {
        m[QStringLiteral("squelch")] = QStringLiteral(
            "not exercised: Auto SQL is engaged and would overwrite every probe");
    } else if (slice) {
        const bool onWas = slice->squelchOn();
        const int levelWas = slice->squelchLevel();
        const int manualWas = slice->manualSquelchLevel();
        const auto restore = qScopeGuard([&] {
            if (!slice)
                return;
            slice->setSquelch(onWas, levelWas);
            slice->setManualSquelchLevel(manualWas);
            spin(600);
        });
        QJsonObject sql = sweep(QStringLiteral("squelch"), 100,
                                [slice] { return !slice.isNull(); },
                                [slice](int v) { slice->setManualSquelch(true, v); },
                                [slice] { return slice->squelchOn() ? slice->squelchLevel() : -1; });
        sql[QStringLiteral("sampleEncoding")] = QStringLiteral("level while on, -1 while off");
        const std::optional<bool> autoAfter = o.autoSquelchEngaged ? o.autoSquelchEngaged()
                                                                   : std::nullopt;
        sql[QStringLiteral("autoSquelchEngaged")] =
            autoBefore ? QJsonValue(*autoBefore) : QJsonValue();
        if (autoAfter.value_or(false)) {
            // Engaged during the sweep: its departures may be Auto's writes.
            problems << QStringLiteral("INCONCLUSIVE — Auto SQL was engaged during the "
                                       "squelch sweep, so its departures may be Auto's");
        } else if (!autoBefore) {
            sql[QStringLiteral("autoSquelchNote")] = QStringLiteral(
                "Auto SQL state not observable from this run; a departure that lands "
                "on the same level for every probe may be Auto rather than a decode");
        }
        m[QStringLiteral("squelch")] = sql;
    }

    if (interrupted) {
        problems.prepend(QStringLiteral("INCONCLUSIVE — the radio, slice or link went "
                                        "away mid-sweep"));
    }
    record(QStringLiteral("control-domain"),
           QStringLiteral("A scaled control reads back where it was written"),
           m,
           QStringLiteral(
               "Every published value is written through the operator's setter "
               "and watched until the radio answers; both ends of the range are "
               "held through two periodic polls. A value that reads back "
               "elsewhere is decoded in a different domain from the one it was "
               "written in, and the next write is built from it."),
           problems.join(QStringLiteral("; ")),
           QStringLiteral("docs/CERTIFICATION.md 1.41"));
}

void RadioCertification::stageFrontEndInterlock()
{
    // A CONTROL THE RADIO CHANGES ON ITS OWN SHOWS AT ONCE.
    //
    // The IC-7300MK2 links its preamp and attenuator: ATT on drops the preamp,
    // preamp on drops ATT, ATT off restores the preamp — and reports none of
    // it. The other button learned it at the next 3 s poll (#6178, fixed by
    // #6183). So each transition below times when the control NOT written
    // reached its final value. One transition can be lucky with the poll
    // phase, so there are several (CERTIFICATION.md 1.42).
    if (!m_radio)
        return;
    const QString panId = m_radio->panId();
    PanadapterModel* pan = panId.isEmpty() ? nullptr : m_radio->panadapter(panId);
    if (!pan || pan->preampLabels().size() < 2 || pan->attenuatorLabels().size() < 2) {
        record(QStringLiteral("front-end-interlock"),
               QStringLiteral("Coupled front-end controls show what the radio did"),
               QJsonObject{{QStringLiteral("exercised"), false},
                           {QStringLiteral("reason"), QStringLiteral(
                                "the pan publishes no stepped preamp and attenuator pair")}},
               QStringLiteral("Nothing to couple."), QString(),
               QStringLiteral("docs/CERTIFICATION.md 1.42"));
        return;
    }

    const int preampWas = pan->preampStep();
    const int attWas = pan->attenuatorStep();
    const QPointer<PanadapterModel> watched(pan);
    // Every write follows an event-loop wait, so the radio or its pan can be
    // gone by then (disconnect, teardown): never write through either.
    auto write = [&](bool preamp, int step) {
        if (!m_radio || !watched)
            return;
        if (preamp)
            m_radio->setPanPreampFor(panId, step);
        else
            m_radio->setPanAttenuatorFor(panId, step);
    };

    // Start from both stages off, so every transition below has a known
    // interlock to expect.
    write(false, 0);
    spin(800);
    write(true, 0);
    spin(1500);

    struct Transition { bool preamp; int step; };
    const Transition transitions[] = {{true, 1}, {false, 1}, {false, 0}, {false, 1}, {true, 1}};
    QJsonArray rows;
    QStringList problems;
    int coupledChanges = 0;
    for (const Transition& t : transitions) {
        if (!watched)
            break;
        const int otherBefore = t.preamp ? watched->attenuatorStep() : watched->preampStep();
        write(t.preamp, t.step);
        std::vector<certmath::TimedSample> own;
        std::vector<certmath::TimedSample> other;
        for (int ms = kControlSampleMs; ms <= kInterlockWatchMs && watched; ms += kControlSampleMs) {
            spin(kControlSampleMs);
            if (!m_radio || !watched)
                break;
            own.push_back({ms, t.preamp ? watched->preampStep() : watched->attenuatorStep()});
            other.push_back({ms, t.preamp ? watched->attenuatorStep() : watched->preampStep()});
        }
        if (own.empty())
            break;
        const int otherAfter = other.back().value;
        const bool coupled = otherAfter != otherBefore;
        const int otherSettled = certmath::settledAtMs(other);
        const QString written = t.preamp ? QStringLiteral("preamp") : QStringLiteral("attenuator");
        const QString coupledName = t.preamp ? QStringLiteral("attenuator") : QStringLiteral("preamp");
        rows.append(QJsonObject{
            {QStringLiteral("wrote"), written},
            {QStringLiteral("step"), t.step},
            {QStringLiteral("ownStep"), own.back().value},
            {QStringLiteral("ownSettledMs"), certmath::settledAtMs(own)},
            {QStringLiteral("coupled"), coupledName},
            {QStringLiteral("coupledBefore"), otherBefore},
            {QStringLiteral("coupledAfter"), otherAfter},
            {QStringLiteral("coupledSettledMs"), coupled ? QJsonValue(otherSettled) : QJsonValue()},
        });
        if (own.back().value != t.step) {
            problems << QStringLiteral("%1 step %2 was written and the pan shows %3")
                            .arg(written).arg(t.step).arg(own.back().value);
        }
        if (coupled) {
            ++coupledChanges;
            if (otherSettled > certmath::kCoupledSettleBudgetMs) {
                problems << QStringLiteral(
                    "writing %1 %2 moved the %3 from %4 to %5 on the radio, and the "
                    "pan showed it after %6 ms — at a periodic poll, not a read "
                    "after the write")
                    .arg(written).arg(t.step).arg(coupledName).arg(otherBefore)
                    .arg(otherAfter).arg(otherSettled);
            }
        }
    }

    // Back where we found it. A stage that is on refuses the other, so restore
    // the one that must end up on last.
    if (attWas > 0) {
        write(true, preampWas);
        spin(800);
        write(false, attWas);
    } else {
        write(false, attWas);
        spin(800);
        write(true, preampWas);
    }
    spin(1500);
    const bool restored = watched && watched->preampStep() == preampWas
                       && watched->attenuatorStep() == attWas;
    if (!watched) {
        // A dropped link, not a refused restore: say which.
        problems.prepend(QStringLiteral("INCONCLUSIVE — the panadapter went away mid-stage; "
                                        "the front end was not restored"));
    } else if (!restored) {
        problems << QStringLiteral("could not restore the front end to preamp %1 / "
                                   "attenuator %2").arg(preampWas).arg(attWas);
    }

    record(QStringLiteral("front-end-interlock"),
           QStringLiteral("Coupled front-end controls show what the radio did"),
           QJsonObject{
               {QStringLiteral("exercised"), true},
               {QStringLiteral("budgetMs"), certmath::kCoupledSettleBudgetMs},
               {QStringLiteral("watchMs"), kInterlockWatchMs},
               {QStringLiteral("transitions"), rows},
               {QStringLiteral("coupledChanges"), coupledChanges},
               {QStringLiteral("restored"), restored},
           },
           QStringLiteral(
               "Each front-end write is watched for 4 s. Where the radio moved the "
               "other stage, the time it showed on the pan says whether the backend "
               "read it after the write or waited for a poll. No coupled change at "
               "all is reported, not judged: not every radio interlocks."),
           problems.join(QStringLiteral("; ")),
           QStringLiteral("docs/CERTIFICATION.md 1.42"));
}

void RadioCertification::stageSquelchScale(const Options& o)
{
    // THE SQL LINE IS DRAWN WHERE THE RADIO'S GATE CLOSES.
    //
    // The line is the published SquelchLevelScale on the pan's dBm axis. The
    // Icom backend published Flex's -160 + level while the IC-7300MK2 gates at
    // about -195 + 1.5·level on its pan, so the line missed by up to 10 dB at
    // the levels squelch is used at (#6180, fixed by #6184). Nothing in the
    // client can see that: the line and Auto SQL both read the same record. So
    // the gate is found by its effect — the audio stops — against a steady
    // carrier whose pan peak is read off the bins the line is drawn on
    // (CERTIFICATION.md 1.43).
    if (!m_radio)
        return;
    // Guarded: every wait below runs the event loop, where a disconnect deletes
    // slices and pans.
    const QPointer<SliceModel> slice(m_radio->slice(0));
    const QString panId = m_radio->panId();
    const QPointer<PanadapterModel> pan(panId.isEmpty() ? nullptr : m_radio->panadapter(panId));
    const std::optional<SquelchLevelScale> scale = m_radio->backendCapabilities().squelchLevelScale;
    const QString id = QStringLiteral("squelch-scale");
    const QString title = QStringLiteral("The SQL line sits where the radio's gate closes");
    const QString ref = QStringLiteral("docs/CERTIFICATION.md 1.43");
    if (!slice || !pan) {
        record(id, title, {}, QStringLiteral("No slice or pan to measure on."),
               QStringLiteral("no slice or panadapter"), ref);
        return;
    }
    if (!scale) {
        record(id, title, QJsonObject{{QStringLiteral("published"), false}},
               QStringLiteral("No scale is published, so no SQL line is drawn and "
                              "Auto SQL is withheld. Nothing to check."),
               QString(), ref);
        return;
    }
    QString mode;
    for (const QString& candidate : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                     QStringLiteral("AM"), QStringLiteral("CW")}) {
        if (scale->appliesTo(candidate)) {
            mode = candidate;
            break;
        }
    }
    if (mode.isEmpty()) {
        record(id, title, QJsonObject{{QStringLiteral("modes"), QJsonArray::fromStringList(scale->modes)}},
               QStringLiteral("The scale covers none of USB/LSB/AM/CW."), QString(), ref);
        return;
    }
    // Auto SQL rewrites the slice's level on every pan frame, so a gate search
    // under it measures Auto, not the radio.
    const std::optional<bool> autoEngaged = o.autoSquelchEngaged ? o.autoSquelchEngaged()
                                                                 : std::nullopt;
    if (autoEngaged.value_or(false)) {
        record(id, title, QJsonObject{{QStringLiteral("autoSquelchEngaged"), true}},
               QStringLiteral("Auto SQL is engaged on the slice under test."),
               QStringLiteral("INCONCLUSIVE — Auto SQL is engaged and would overwrite "
                              "every probe; turn it off and re-run"),
               ref);
        return;
    }

    const double carrierMhz = o.squelchCarrierMhz > 0.0 ? o.squelchCarrierMhz
                                                        : o.referenceCarrierMhz;
    const double freqWas = slice->frequency();
    const QString modeWas = slice->mode();
    const bool onWas = slice->squelchOn();
    const int levelWas = slice->squelchLevel();
    const int manualWas = slice->manualSquelchLevel();
    bool interrupted = false;
    const auto restore = qScopeGuard([&] {
        if (!slice)
            return;
        slice->setSquelch(onWas, levelWas);
        slice->setManualSquelchLevel(manualWas);
        slice->setMode(modeWas);
        slice->setFrequency(freqWas);
        spin(800);
    });

    // Pan peak near the carrier and the pan floor, median of every frame in
    // the window, read from the bins the SQL line is drawn against.
    auto readPan = [&](double targetHz, int ms) {
        std::vector<double> peaks;
        std::vector<double> floors;
        const QPointer<PanadapterModel> p(pan);
        if (!m_radio)
            return std::pair{std::nan(""), std::nan("")};
        const auto c = QObject::connect(m_radio, &RadioModel::panFeedSpectrumReady, m_radio,
            [&, p](quint32 streamId, const QVector<float>& bins, qint64) {
                if (!p || streamId != p->panStreamId() || bins.isEmpty())
                    return;
                const double bwHz = p->bandwidthMhz() * 1e6;
                const double lowHz = p->centerMhz() * 1e6 - bwHz / 2.0;
                const std::vector<float> v(bins.cbegin(), bins.cend());
                const double binHz = bwHz / static_cast<double>(v.size());
                peaks.push_back(certmath::panPeakNear(v, lowHz, lowHz + bwHz, targetHz,
                                                      std::max(500.0, 2.0 * binHz)));
                floors.push_back(certmath::median(std::vector<double>(v.cbegin(), v.cend())));
            });
        spin(ms);
        QObject::disconnect(c);
        return std::pair{certmath::median(peaks), certmath::median(floors)};
    };

    // RMS of the audio the operator hears.
    auto audioDb = [&](int ms) {
        double sum = 0.0;
        qint64 n = 0;
        if (!m_radio)
            return -999.0;
        const auto c = QObject::connect(m_radio, &RadioModel::rxDemodAudioReady, m_radio,
            [&](const PcmFrame& pcm) {
                for (const float s : pcm.samples()) {
                    sum += static_cast<double>(s) * s;
                    ++n;
                }
            });
        spin(ms);
        QObject::disconnect(c);
        return n > 0 ? db(std::sqrt(sum / static_cast<double>(n))) : -999.0;
    };

    QJsonArray probes;
    auto findGate = [&](double openDb) {
        auto closedAt = [&](int level) {
            if (!slice) {
                // Ends the search quickly; the result is discarded below.
                interrupted = true;
                return true;
            }
            slice->setSquelch(true, level);
            spin(900);
            const double d = audioDb(700);
            const bool closed = certmath::gateClosed(openDb, d);
            probes.append(QJsonObject{{QStringLiteral("level"), level},
                                      {QStringLiteral("audioDb"), d},
                                      {QStringLiteral("closed"), closed}});
            return closed;
        };
        // From 1: a radio with no squelch enable cannot gate at threshold 0.
        int gate = certmath::lowestClosedLevel(1, 100, closedAt);
        // Confirm with a one-level bracket, not a repeat at the gate itself:
        // there the carrier sits on the threshold and the gate chatters
        // (measured on the MK2: closed, then open 6 dB down, at one level).
        // At 100 there is no level above, so only the level below can confirm.
        bool confirmed = gate >= 1;
        if (gate == 100)
            confirmed = !closedAt(99);
        else if (gate >= 1)
            confirmed = closedAt(gate + 1) && (gate == 1 || !closedAt(gate - 1));
        return std::pair{gate, confirmed};
    };

    slice->setMode(mode);
    const double offsetMhz = o.referenceOffsetHz / 1.0e6;
    const double dialMhz = mode == QLatin1String("LSB") ? carrierMhz + offsetMhz
                                                        : carrierMhz - offsetMhz;
    slice->setFrequency(dialMhz);
    slice->setSquelch(false, levelWas);
    spin(2500);
    const auto [peakDb, floorDb] = readPan(carrierMhz * 1e6, 2000);
    const QJsonObject levels = meterSnapshot();
    const double openDb = audioDb(1000);

    QJsonObject m{
        {QStringLiteral("published"), true},
        {QStringLiteral("offsetDb"), scale->offsetDb},
        {QStringLiteral("dbPerStep"), scale->dbPerStep},
        {QStringLiteral("autoSquelch"), scale->autoSquelch},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("carrierMhz"), carrierMhz},
        {QStringLiteral("dialMhz"), dialMhz},
        {QStringLiteral("preamp"), pan ? QJsonValue(pan->preampStep()) : QJsonValue()},
        {QStringLiteral("attenuator"), pan ? QJsonValue(pan->attenuatorStep()) : QJsonValue()},
        {QStringLiteral("autoSquelchEngaged"), autoEngaged ? QJsonValue(*autoEngaged) : QJsonValue()},
        {QStringLiteral("carrierPanPeakDb"), std::isfinite(peakDb) ? QJsonValue(peakDb) : QJsonValue()},
        {QStringLiteral("panFloorDb"), std::isfinite(floorDb) ? QJsonValue(floorDb) : QJsonValue()},
        {QStringLiteral("sLevelDbm"), levels.value(QStringLiteral("sLevelDbm"))},
        {QStringLiteral("openAudioDb"), openDb},
        {QStringLiteral("toleranceDb"), certmath::kSquelchLineToleranceDb},
    };
    QStringList problems;
    QString inconclusive;
    const double sLevel = levels.value(QStringLiteral("sLevelDbm")).toDouble(-999.0);
    if (!std::isfinite(peakDb)) {
        inconclusive = QStringLiteral("no pan bins near the carrier — is it on the pan?");
    } else if (sLevel > certmath::kS9Dbm) {
        // Above S9 Flex's scale and the MK2's measured one miss the gate by
        // similar amounts, so a pass here would certify either.
        inconclusive = QStringLiteral("the carrier reads %1 dBm, above S9, where a "
                                      "wrong scale lands near the gate too — choose a "
                                      "weaker carrier or add attenuation")
                           .arg(sLevel, 0, 'f', 1);
    } else if (openDb < -90.0) {
        inconclusive = QStringLiteral("no audio with squelch open, so a closed gate "
                                      "cannot be told from silence");
    } else {
        const auto [gate, confirmed] = findGate(openDb);
        m[QStringLiteral("gateLevel")] = gate;
        m[QStringLiteral("gateConfirmed")] = confirmed;
        if (interrupted) {
            inconclusive = QStringLiteral("the slice went away mid-search");
        } else if (gate < 1) {
            inconclusive = QStringLiteral("the gate is still open at level 100 — the "
                                          "carrier is above the whole scale");
        } else {
            const double lineDb = scale->thresholdDb(gate);
            const double errorDb = lineDb - peakDb;
            m[QStringLiteral("lineAtGateDb")] = lineDb;
            m[QStringLiteral("lineMinusCarrierDb")] = errorDb;
            if (!confirmed) {
                inconclusive = QStringLiteral("the gate at level %1 did not bracket (closed "
                                              "one above, open one below) — a fading "
                                              "carrier or a non-monotonic gate")
                                   .arg(gate);
            } else if (std::fabs(errorDb) > certmath::kSquelchLineToleranceDb) {
                problems << QStringLiteral(
                    "the radio's squelch closed on this carrier at level %1, where the "
                    "SQL line is drawn at %2 dB, but the carrier peaks at %3 dB on the "
                    "pan: the line is %4 dB %5 the gate")
                    .arg(gate).arg(lineDb, 0, 'f', 1).arg(peakDb, 0, 'f', 1)
                    .arg(std::fabs(errorDb), 0, 'f', 1)
                    .arg(errorDb > 0 ? QStringLiteral("above") : QStringLiteral("below"));
            }
        }
    }
    m[QStringLiteral("carrierProbes")] = probes;

    // AUTO SQL picks floor + margin (5..20 dB) on the pan. It can only hold a
    // margin if the radio gates band noise inside that window above the floor.
    if (scale->autoSquelch && inconclusive.isEmpty() && slice) {
        probes = QJsonArray{};
        const double noiseDialMhz = dialMhz + 0.010;   // carrier out of the passband
        slice->setFrequency(noiseDialMhz);
        slice->setSquelch(false, levelWas);
        spin(2500);
        const double noiseFloorDb = readPan(noiseDialMhz * 1e6, 2000).second;
        const double noiseOpenDb = audioDb(1000);
        // The carrier branch's silence guard, for the same reason: a near-silent
        // passband "closes" at any level and would report a gate from nothing.
        const bool noiseAudible = noiseOpenDb >= -90.0;
        const auto [noiseGate, noiseConfirmed] = noiseAudible ? findGate(noiseOpenDb)
                                                              : std::pair{-1, false};
        QJsonObject a{
            {QStringLiteral("dialMhz"), noiseDialMhz},
            {QStringLiteral("panFloorDb"), std::isfinite(noiseFloorDb) ? QJsonValue(noiseFloorDb) : QJsonValue()},
            {QStringLiteral("openAudioDb"), noiseOpenDb},
            {QStringLiteral("noiseGateLevel"), noiseGate},
            {QStringLiteral("noiseGateConfirmed"), noiseConfirmed},
            {QStringLiteral("probes"), probes},
        };
        if (!noiseAudible) {
            a[QStringLiteral("inconclusive")] = QStringLiteral(
                "no band-noise audio with squelch open; the noise gate cannot be found");
        } else if (noiseGate >= 1 && noiseConfirmed && !interrupted
                   && std::isfinite(noiseFloorDb)) {
            const double aboveFloorDb = scale->thresholdDb(noiseGate) - noiseFloorDb;
            a[QStringLiteral("noiseGateAboveFloorDb")] = aboveFloorDb;
            if (aboveFloorDb > 20.0) {
                problems << QStringLiteral(
                    "Auto SQL is offered, but the radio gates band noise %1 dB above "
                    "the pan floor: at its widest 20 dB margin Auto leaves the noise "
                    "open").arg(aboveFloorDb, 0, 'f', 1);
            }
        }
        m[QStringLiteral("autoSquelchCheck")] = a;
    }
    if (!inconclusive.isEmpty())
        problems.prepend(QStringLiteral("INCONCLUSIVE — ") + inconclusive);

    record(id, title, m,
           QStringLiteral(
               "Binary-search the squelch level at which the audio from a steady "
               "carrier stops, then compare the published line at that level with "
               "the carrier's pan peak. Report the carrier's S-meter reading too: "
               "above S9 a correct record can still sit several dB off."),
           problems.join(QStringLiteral("; ")), ref);
}

void RadioCertification::stageMeterInventory()
{
    // Are the meters even connected? IRadioBackend::meterUpdate had NO consumer
    // anywhere for the whole receive bring-up, so every value this backend
    // computed was discarded and the S-meter was correct for days without ever
    // being visible. A meter that is defined but never updates looks identical
    // to a quiet band.
    if (!m_radio)
        return;
    const auto& meters = m_radio->meterModel();
    QJsonObject m;
    QStringList definedNeverFed, expectedButMissing, unitMismatches, inconclusive;

    // kMeterTable's `expectedOnHl2` column is a fact about ONE radio class.
    const bool tableAppliesToThisRadio =
        m_radio->family().compare(QStringLiteral("hl2"), Qt::CaseInsensitive) == 0;

    // DID THIS RUN ACTUALLY TRANSMIT? Established from forward power observed
    // inside a keyed window — a quantity independent of the meters being judged
    // — because with the operator's drive slider at zero every keyed stage
    // succeeded, keyRefusals was 0, every precondition passed, and the radio
    // radiated nothing. The report then blamed TX:SWR and recommended deleting
    // it (CERTIFICATION.md 1.37).
    const bool rfConfirmed = keyedRfConfirmed();
    const bool anyKeyedWindow = m_keyedWindows > 0;
    // ASK THE MODEL, do not report the observation flag. m_fwdPowerMeterDefined
    // is only set when a keyed window actually looked, so on a run where every
    // key was refused it reads false — which states as a fact about the radio
    // ("it defines no forward-power meter") something that is really a fact
    // about the run ("nothing ever looked"). The distinction is the whole
    // subject of this stage.
    const bool fwdMeterDefined =
        meters.findMeter(QStringLiteral("TX"), QStringLiteral("FWDPWR")) >= 0;
    m[QStringLiteral("keyedRf")] = QJsonObject{
        {QStringLiteral("confirmed"), rfConfirmed},
        {QStringLiteral("keyedWindows"), m_keyedWindows},
        {QStringLiteral("freshForwardPowerSamples"), m_keyedRfSamples},
        {QStringLiteral("maxWattsWhileKeyed"),
         m_keyedRfSamples > 0 ? QJsonValue(m_keyedFwdWattsMax) : QJsonValue()},
        {QStringLiteral("floorWatts"), static_cast<double>(kKeyedRfFloorWatts)},
        {QStringLiteral("forwardPowerMeterDefined"), fwdMeterDefined},
        {QStringLiteral("keyRefusals"), m_keyRefusals},
    };

    for (const MeterSpec& spec : kMeterTable) {
        const QString key = QString::fromLatin1(spec.source) + QLatin1Char(':')
                          + QString::fromLatin1(spec.name);
        const int idx = meters.findMeter(QString::fromLatin1(spec.source),
                                         QString::fromLatin1(spec.name));
        const qint64 age = idx >= 0 ? meters.valueAgeMs(idx) : -1;
        // `acceptedUnits` (from kMeterSurfaces) is every unit the consumer handles;
        // `declaredUnit` is what the backend published. A declaration outside the
        // set means the meter is already misread however fresh it is (e.g. FWDPWR in
        // W against a dBm consumer).
        const MeterDef* def = idx >= 0 ? meters.meterDef(idx) : nullptr;
        const QString declared = def ? def->unit : QString();
        const MeterSurface* surface = meterSurfaceFor(key);
        const QString accepted =
            surface ? QString::fromLatin1(surface->acceptedUnits) : QString();
        const bool unitDisagrees = def && surface
            && !meterUnitAccepted(accepted, declared);
        if (unitDisagrees)
            unitMismatches << QStringLiteral("%1 (declared %2, consumer handles %3)")
                                  .arg(key, declared, accepted);

        // INCONCLUSIVE, not "never fed". A meter cannot be judged by a run that
        // never produced the thing it measures, and NOT-TESTED is already a
        // first-class outcome in the control scrub (§1.29). Two rungs: a key
        // that never went down leaves even the host-side transmit meters
        // unexercised; a key that went down silently additionally leaves the RF
        // ones unexercised.
        QString untestedBecause;
        if (spec.needsKey && !anyKeyedWindow) {
            untestedBecause = m_keyRefusals > 0
                ? QStringLiteral("the radio refused every key this run (%1 refusals), "
                                 "so nothing was ever transmitted for this meter to "
                                 "measure").arg(m_keyRefusals)
                : QStringLiteral("no stage in this run keyed the transmitter, so "
                                 "nothing was ever transmitted for this meter to "
                                 "measure");
        } else if (spec.needsForwardPower && !rfConfirmed) {
            untestedBecause = QStringLiteral(
                "no keyed stage in this run produced forward power above %1 W, so "
                "this meter had nothing to report")
                .arg(static_cast<double>(kKeyedRfFloorWatts), 0, 'g', 3);
        }

        QJsonObject row{
            {QStringLiteral("acceptedUnits"), accepted},
            {QStringLiteral("declaredUnit"), declared},
            {QStringLiteral("unitDisagrees"), unitDisagrees},
            {QStringLiteral("defined"), idx >= 0},
            {QStringLiteral("everFed"), age >= 0},
            {QStringLiteral("ageMs"), static_cast<double>(age)},
            {QStringLiteral("needsKey"), spec.needsKey},
            {QStringLiteral("needsForwardPower"), spec.needsForwardPower},
            // Named for the radio class it describes, not "this radio" — the
            // old key claimed the table had been evaluated against whatever
            // backend happened to be connected.
            {QStringLiteral("expectedOnHl2Class"), spec.expectedOnHl2},
            // fromUtf8, not fromLatin1: these notes are UTF-8 source literals
            // and Latin-1 decoding mangled the one em dash in the table into
            // three characters in the report.
            {QStringLiteral("note"), QString::fromUtf8(spec.note)},
        };
        const bool untested = !untestedBecause.isEmpty();
        if (untested && idx >= 0 && age < 0) {
            row[QStringLiteral("verdict")] = QStringLiteral("INCONCLUSIVE");
            row[QStringLiteral("inconclusiveReason")] = untestedBecause;
        }
        m[key] = row;

        // Defined but never fed renders as a real instrument reading nothing,
        // which is worse than an absent one: the operator has no way to tell it
        // apart from a quiet band.
        // By the time this runs, the stages above have keyed and injected
        // audio, so a meter with no value has genuinely never been fed rather
        // than merely not exercised — PROVIDED those stages transmitted. When
        // they did not, the silence says nothing about the meter and this is an
        // inconclusive result, not a negative one.
        if (idx >= 0 && age < 0) {
            if (untested)
                inconclusive << key;
            else
                definedNeverFed << key;
        }
        if (tableAppliesToThisRadio && spec.expectedOnHl2 && idx < 0)
            expectedButMissing << key;
    }
    m[QStringLiteral("expectationsApplied")] = tableAppliesToThisRadio;

    QString concern;
    if (!tableAppliesToThisRadio) {
        // THE EXPECTATIONS ARE ONE RADIO'S, so do not apply them to another and
        // present the result as a verdict. Run against a Flex, FWDPWR/REFPWR/ALC
        // are real published meters marked expected=false here, so the inventory
        // would have reported a clean bill of health for meters it never checked
        // for — a confident verdict derived from a different radio's
        // expectations, which is the exact shape of error this tool exists to
        // catch. The per-meter defined/everFed data below is measured and stays
        // valid on any backend.
        concern = QStringLiteral(
            "the expected-meter table is Hermes-Lite 2 specific and this radio "
            "reports family '%1', so no expectation was applied. The per-meter "
            "defined/everFed readings below are still measured and valid; add a "
            "family column (docs/CERTIFICATION.md 2.1 — radio profile) to get an "
            "inventory verdict on this radio").arg(m_radio->family());
    } else if (!expectedButMissing.isEmpty()) {
        concern = QStringLiteral("expected on this radio but not defined: ")
                + expectedButMissing.join(QStringLiteral(", "));
    }
    if (!definedNeverFed.isEmpty()) {
        if (!concern.isEmpty()) concern += QStringLiteral(". ");
        concern += QStringLiteral(
            "DEFINED BUT NEVER FED — these render as instruments reading a quiet "
            "band and cannot be told apart from one: ")
            + definedNeverFed.join(QStringLiteral(", "))
            + QStringLiteral(". Either publish them with a documented scale or "
                             "stop defining them");
    }
    // INCONCLUSIVE COMES BEFORE THE NEGATIVE FINDINGS, and is worded as a
    // statement about the RUN rather than about the meters. A reader who skims
    // must not come away with "TX:SWR is broken" from a run that never keyed
    // the PA, because the remedy the negative form recommends is deleting a
    // working meter.
    if (!inconclusive.isEmpty()) {
        QString why;
        if (m_keyRefusals > 0 && !anyKeyedWindow) {
            // §1.17: a refused action reads as a broken subject. Name the
            // refusal, because it is the actionable fact and the meters are
            // not the subject of it.
            why = QStringLiteral(
                "the radio refused every key this run (%1 refusals) — enable TX "
                "automation, or check band limits and interlocks")
                .arg(m_keyRefusals);
        } else if (!anyKeyedWindow) {
            why = QStringLiteral("no stage in this run keyed the transmitter");
        } else if (!fwdMeterDefined) {
            why = QStringLiteral(
                "this radio defines no TX:FWDPWR meter, so there is no quantity "
                "independent of the meters below that can confirm a transmission");
        } else if (m_keyedRfSamples == 0) {
            why = QStringLiteral(
                "forward power was never fresh inside a keyed window across %1 "
                "keyed stage(s)").arg(m_keyedWindows);
        } else {
            why = QStringLiteral(
                "forward power peaked at %1 W across %2 keyed stage(s), below the "
                "%3 W floor an SWR reading needs behind it — the RF power slider "
                "at 0 does this, and so do an interlock, a band limit and a PA "
                "that never enabled")
                .arg(m_keyedFwdWattsMax, 0, 'g', 3)
                .arg(m_keyedWindows)
                .arg(static_cast<double>(kKeyedRfFloorWatts), 0, 'g', 3);
        }
        const QString lead = QStringLiteral(
            "INCONCLUSIVE — this run produced no confirmed RF, so nothing here is "
            "a verdict on these meters: ")
            + inconclusive.join(QStringLiteral(", "))
            + QStringLiteral(". ") + why
            + QStringLiteral(". Re-run with drive up and into a load before "
                             "concluding anything about them");
        concern = concern.isEmpty() ? lead : lead + QStringLiteral(". ") + concern;
    }

    // A UNIT MISMATCH OUTRANKS EVERYTHING ELSE HERE, so it goes first. Every
    // other concern in this stage describes a meter that is not there; this one
    // describes a meter that IS there, is fresh, and is wrong — which is the
    // only failure mode in the set that looks healthy from every angle the
    // stage previously had.
    if (!unitMismatches.isEmpty()) {
        QString lead = QStringLiteral(
            "UNIT MISMATCH — these are defined, fed and MISREAD. The consumer "
            "converts by the unit it expects, not the one the backend declared: ")
            + unitMismatches.join(QStringLiteral(", "));
        concern = concern.isEmpty() ? lead : lead + QStringLiteral(". ") + concern;
    }

    // What the gauge will show, read through the converting consumers
    // (CERTIFICATION.md 1.27). Sampled while keyed by stageMeterScale, behind each
    // consumer's liveness gate; unkeyed every TX quantity is absent and raw SWR
    // reads its 1.0f initialiser (1.34). The unkeyed reading is kept to show the
    // gauge falls back correctly.
    QJsonObject rendered = m_renderedWhileKeyed;
    const bool haveKeyedSample = !rendered.isEmpty();
    if (!haveKeyedSample)
        rendered = renderedSnapshot();
    rendered[QStringLiteral("sampledWhileKeyed")] = haveKeyedSample;
    m[QStringLiteral("asRendered")] = rendered;
    m[QStringLiteral("asRenderedUnkeyed")] = renderedSnapshot();

    record(QStringLiteral("meter-inventory"),
           QStringLiteral("Meters exist and actually receive values"),
           m,
           QStringLiteral(
               "Definition and delivery are separate wires and were connected "
               "separately. A defined meter that never updates is indistinguish"
               "able from a real reading of nothing — and from a meter that was "
               "never given anything to measure, which is why the RF-dependent "
               "rows are only judged once a keyed stage has been confirmed to "
               "produce forward power."),
           concern,
           QStringLiteral("docs/HERMES.md 14.4 — the orphaned meter seam"));
}

void RadioCertification::stageMeterScale(const Options& o)
{
    // SCALE, not just presence. A meter can be wired, fresh, and still wrong:
    // this radio published raw dBFS onto a dBm axis, and SWR once pegged at
    // 255.99:1 while idle because the ratio saturated the fixed-point range.
    //
    // Two checks with a KNOWN answer:
    //   - a -20 dBFS test tone must read about -20 on the mic peak meter
    //   - a dummy load must read about 1.0:1 on SWR
    // Neither is a guess about this radio; both are arithmetic.
    if (!m_audio || !m_radio)
        return;

    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }
    const bool keyedOk = keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject keyed = meterSnapshot();
    // SAMPLE THE CONSUMER HERE, WHILE THE KEY IS DOWN. This is the only moment
    // in the meters phase when a transmit-only quantity can exist, and the
    // inventory stage that reports it runs after stageControlEffect has unkeyed
    // and settled 700 ms — so it was reading the one instant the value is
    // guaranteed absent, and reported 0.001 W in a run that measured 2.0 W
    // (CERTIFICATION.md 1.39).
    if (keyedOk) {
        m_renderedWhileKeyed = renderedSnapshot();
        observeKeyedRf();
    }
    keyViaOperatorPath(false);
    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    spin(900);

    const double micPeak = keyed.value(QStringLiteral("micPeakDbfs")).toDouble(-999.0);
    const double swr = keyed.value(QStringLiteral("swr")).toDouble(-1.0);
    QJsonObject m{
        {QStringLiteral("injectedToneDbfs"), -20.0},
        {QStringLiteral("micPeakDbfs"), micPeak},
        {QStringLiteral("micPeakErrorDb"), micPeak + 20.0},
        {QStringLiteral("swr"), swr},
        {QStringLiteral("swrStale"), keyed.contains(QStringLiteral("swrStale"))},
        // The SWR row above is only a measurement of anything if RF happened.
        // `swr: -1` at zero drive is the absence of a transmission, not the
        // absence of a meter, and this is where a reader finds out which.
        {QStringLiteral("keyedRfConfirmed"), keyedRfConfirmed()},
        {QStringLiteral("maxFwdWattsWhileKeyed"),
         m_keyedRfSamples > 0 ? QJsonValue(m_keyedFwdWattsMax) : QJsonValue()},
    };

    QStringList problems;
    if (micPeak < -998.0)
        problems << QStringLiteral("no mic peak reading while transmitting a tone");
    else if (std::fabs(micPeak + 20.0) > 3.0)
        problems << QStringLiteral(
            "mic peak reads %1 dBFS for a -20 dBFS tone — a scale error of %2 dB")
            .arg(micPeak, 0, 'f', 1).arg(micPeak + 20.0, 0, 'f', 1);
    if (swr > 50.0)
        problems << QStringLiteral(
            "SWR reads %1 into a load — a value that large usually means the "
            "ratio saturated its fixed-point range rather than a real mismatch")
            .arg(swr, 0, 'f', 1);

    record(QStringLiteral("meter-scale"),
           QStringLiteral("Meters read the right NUMBER, not just a number"),
           m,
           QStringLiteral(
               "Checks with a known answer: a -20 dBFS injected tone should read "
               "-20 on the mic peak meter, and a dummy load should read near "
               "1.0:1. Absolute POWER is deliberately not checked — the counts "
               "are uncalibrated and presenting them as watts is the error this "
               "project already avoided once."),
           problems.join(QStringLiteral("; ")),
           QStringLiteral("docs/HERMES.md 14.4; SWR gating and the dB reference"));
}

void RadioCertification::stageTuning(const Options& o)
{
    // Control plane only — no DSP, no audio, no meters. If the radio is not
    // where it is said to be, nothing measured afterwards means anything, and
    // this is the cheapest thing to get wrong: the TX oscillator once sat on the
    // previous session's frequency while the VFO read the new one.
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    // KEYED BY STEP INDEX, NOT BY FREQUENCY STRING. `radiocert tune 7.1` makes
    // the list {7.1, 8.1, 7.1, 3.7} and two steps collide on the key "7.1000",
    // so one measurement silently overwrites the other — a diagnostic quietly
    // reporting fewer results than it took.
    QJsonObject perFreq;
    int step = 0;
    for (const double mhz : {o.frequencyMhz, o.frequencyMhz + 1.0, 7.100, 3.700}) {
        slice->setFrequency(mhz);
        spin(900);
        perFreq[QStringLiteral("step%1_%2MHz").arg(step++).arg(mhz, 0, 'f', 4)]
            = QJsonObject{
            {QStringLiteral("requestedMhz"), mhz},
            {QStringLiteral("readbackMhz"), slice->frequency()},
            {QStringLiteral("errorHz"), (slice->frequency() - mhz) * 1.0e6},
        };
    }
    slice->setFrequency(o.frequencyMhz);
    spin(600);

    QStringList bad;
    for (auto it = perFreq.begin(); it != perFreq.end(); ++it) {
        if (std::fabs(it.value().toObject().value(QStringLiteral("errorHz")).toDouble()) > 1.0)
            bad << it.key();
    }

    record(QStringLiteral("tuning"),
           QStringLiteral("The dial goes where it is told, across bands"),
           perFreq,
           QStringLiteral(
               "Several frequencies across bands, including ones far enough apart "
               "to force a DDC re-centre. Readback only proves the model agrees — "
               "a radio that reports no VFO cannot be asked where it really is."),
           bad.isEmpty() ? QString()
                         : QStringLiteral("these did not take: ") + bad.join(", "),
           QStringLiteral("docs/HERMES.md 14.1 — connect-time state"));
}

void RadioCertification::stageModeMap()
{
    // A mode name the backend does not recognise falls through to a default —
    // silently. On the HL2, plain "CW" was absent while "CWU" was present, so CW
    // was demodulated as SSB with the mode indicator reading correctly, and
    // "RTTY" is advertised over TCI to this day with no mapping behind it.
    //
    // This enumerates every mode the application can actually emit and asks the
    // slice to take each one. It cannot see inside the backend's lookup, so what
    // it reports is the readback — but a mode that does not survive a round trip
    // is certainly wrong, and one that does is at least addressable.
    static const QStringList kModes{
        QStringLiteral("USB"), QStringLiteral("LSB"),
        QStringLiteral("CW"),  QStringLiteral("CWU"), QStringLiteral("CWL"),
        QStringLiteral("AM"),  QStringLiteral("SAM"),
        QStringLiteral("FM"),  QStringLiteral("NFM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"),
        QStringLiteral("RTTY"),
    };

    QJsonObject results;
    QStringList notRetained;
    auto* slice = m_radio ? m_radio->slice(0) : nullptr;
    const QString restore = slice ? slice->mode() : QString();

    for (const QString& mode : kModes) {
        if (!slice)
            break;
        slice->setMode(mode);
        spin(250);
        const QString back = slice->mode();
        // Record the resulting passband as a FINGERPRINT. A mode the backend
        // does not recognise falls through to its default, so a passband
        // identical to the fallback mode's is evidence of that — weak evidence,
        // but the only kind available from out here, and better than a readback
        // that any string survives.
        results[mode] = QJsonObject{
            {QStringLiteral("readback"), back},
            {QStringLiteral("passband"),
             QStringLiteral("%1..%2").arg(slice->filterLow()).arg(slice->filterHigh())},
        };
        // A differing readback is either an unmapped mode falling to the backend
        // default or a deliberate alias (HL2's hl2::canonicalOfferedMode publishes
        // "CWU" as "CW", "NFM" as "FM"); this tool can't tell them apart, so the
        // concern names both.
        if (back.compare(mode, Qt::CaseInsensitive) != 0)
            notRetained << (mode + QStringLiteral("->") + back);
    }
    if (slice && !restore.isEmpty())
        slice->setMode(restore);

    QString concern;
    if (!notRetained.isEmpty())
        concern = QStringLiteral(
            "these modes did not survive a round trip: ") + notRetained.join(", ")
            + QStringLiteral(". Each is one of two things and this stage cannot "
                             "tell which: a mode the backend does not map, which "
                             "does not fail but becomes the default, usually USB, "
                             "while the UI still shows what was asked for — or a "
                             "deliberate alias collapse onto the spelling the mode "
                             "menu carries (on the HL2, CWU->CW and NFM->FM are "
                             "this, and are correct). Read the passband "
                             "fingerprint beside each: an alias keeps its own "
                             "window, a fallback takes the default mode's");

    record(QStringLiteral("mode-map"),
           QStringLiteral("Every mode the app can emit survives a round trip"),
           results,
           QStringLiteral(
               "READBACK IS NOT PROOF. The slice keeps whatever string the "
               "BACKEND publishes, which for most modes is the string it was "
               "given — so every mode can come back clean while the backend "
               "maps only some of them and silently demodulates the rest as "
               "its default. Confirmed on this radio: RTTY survives the round "
               "trip and the backend has no mapping for it. A readback that "
               "DIFFERS is not automatically a fault either: a backend may "
               "collapse an alias onto the spelling its mode menu carries, "
               "which the HL2 does for CWU->CW and NFM->FM. Compare each "
               "passband against the fallback mode's — an exact match is a "
               "hint, and the only one visible from outside the backend."),
           concern,
           QStringLiteral("docs/HERMES.md 15.7"));
}

void RadioCertification::stageConsumerAgreement(const Options& o)
{
    // The panadapter and the demodulator are INDEPENDENT consumers of the same
    // IQ buffer, and on the HL2 each was wired to the other's convention. The
    // audio was right at normal tuning and the panadapter was visibly mirrored —
    // and the panadapter, the one instrument with no compensating error, was the
    // easiest to dismiss as "a display bug".
    //
    // This stage does not try to decide which is right. It reports whether they
    // agree, because a disagreement means one of them is compensating for
    // something and that is the fact worth surfacing.
    if (!m_radio)
        return;

    QJsonObject m{
        {QStringLiteral("note"), QStringLiteral(
            "Requires a signal OFF the pan centre. A mirror is invisible on its "
            "own axis, which is how a live sideband sweep once confirmed 'all "
            "four modes correct' while the display was plainly mirrored.")},
        {QStringLiteral("referenceCarrierMhz"), o.referenceCarrierMhz},
        {QStringLiteral("dialOffsetHz"), o.referenceOffsetHz},
    };

    record(QStringLiteral("consumer-agreement"),
           QStringLiteral("Panadapter and demodulator agree on which side a signal is"),
           m,
           QStringLiteral(
               "Park a known carrier off-centre, then compare where the "
               "panadapter draws it against which sideband recovers it. They are "
               "independent consumers and can disagree."),
           QStringLiteral(
               "NOT YET AUTOMATED — needs a spectrum tap through the seam. Until "
               "then this is an operator check, and it is the one that found the "
               "receive inversion"),
           QStringLiteral("docs/HERMES.md 15.5"));
}

void RadioCertification::stageZeroShift(const Options& o)
{
    // Two compensating errors cancel at any non-zero shift, so a measurement
    // taken at normal off-centre tuning sees a corrected result and proves
    // nothing. Zero shift is the one geometry where nothing can compensate.
    //
    // Force it by exploiting the NCO re-centre rule: tune far enough that the
    // NCO must jump, then land on the target, and the NCO follows it exactly.
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    const double target = o.referenceCarrierMhz - (o.referenceOffsetHz / 1.0e6);
    slice->setFrequency(target - 3.0);   // far: forces the NCO to move
    spin(1200);
    slice->setFrequency(target);         // land: NCO re-centres, shift == 0
    spin(1500);

    QJsonObject m{
        {QStringLiteral("dialMhz"), slice->frequency()},
        {QStringLiteral("intendedMhz"), target},
        {QStringLiteral("method"), QStringLiteral(
            "tuned 3 MHz away to force an NCO jump, then landed on the target so "
            "the NCO follows and the slice shift is exactly zero")},
    };

    record(QStringLiteral("zero-shift"),
           QStringLiteral("Establish the zero-shift geometry"),
           m,
           QStringLiteral(
               "Any handedness measurement taken at a non-zero shift can be "
               "corrected by a second, opposite error. This puts the radio in the "
               "one geometry where that cannot happen — do the sideband checks "
               "from here."),
           slice->frequency() > 0.0 ? QString()
                                    : QStringLiteral("could not establish a dial frequency"),
           QStringLiteral("docs/HERMES.md 15.4"));
}

void RadioCertification::stageRxSidebands(const Options& o)
{
    // All FOUR SSB-family modes against a known off-centre carrier. Not one mode,
    // and not at the pan centre.
    //
    // A sideband test that runs in a single mode proves nothing about handedness:
    // hl2_shift_test validated in LSB, the one mode the inversion made correct,
    // and passed throughout.
    if (!m_audio || !m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    QJsonObject perMode;
    for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                QStringLiteral("DIGU"), QStringLiteral("DIGL")}) {
        slice->setMode(mode);
        spin(900);
        m_audio->startAutomationAudioCapture(1500,
                                             QStringList{QStringLiteral("output")});
        spin(1700);
        const QJsonObject snap = m_audio->automationAudioCaptureSnapshot(true);

        // Take the rate from the CAPTURE, never from a constant.
        //
        // This originally used AudioEngine::DEFAULT_SAMPLE_RATE (24 kHz) while
        // the "output" tap runs at the audio DEVICE's rate — 48 kHz here. The
        // correlator therefore probed the wrong frequency and reported -80 to
        // -109 dB for every mode while the RMS plainly showed a 25 dB signal.
        // A tone detector that silently looks in the wrong place is worse than
        // no tone detector, because it reads as "no signal".
        double fs = 0.0;
        std::vector<float> mono;
        for (const QJsonValue& cv : snap.value(QStringLiteral("chunks")).toArray()) {
            const QJsonObject c = cv.toObject();
            const int ch = std::max(1, c.value(QStringLiteral("channels")).toInt(1));
            fs = c.value(QStringLiteral("sampleRate")).toDouble(fs);
            const QByteArray pcm = QByteArray::fromBase64(
                c.value(QStringLiteral("pcmBase64")).toString().toLatin1());
            const auto* f = reinterpret_cast<const float*>(pcm.constData());
            const int frames = static_cast<int>(pcm.size() / sizeof(float)) / ch;
            for (int n = 0; n < frames; ++n)
                mono.push_back(f[n * ch]);
        }
        if (fs <= 0.0)
            fs = AudioEngine::DEFAULT_SAMPLE_RATE;   // last resort, and reported

        // SEARCH A BAND, NOT A BIN. The reference carrier's frequency is exact;
        // our dial's is not. See tonePowerNear() — a coherent 1.5 s integration
        // is a ~0.67 Hz bin, and a 1 ppm error at 10 MHz lands ~10 Hz away, so
        // every mode reads the noise floor at once and the report says "deaf"
        // when it means "missed".
        perMode[mode] = QJsonObject{
            {QStringLiteral("sampleRateHz"), fs},
            {QStringLiteral("toneSearchSpanHz"), kReferenceSearchSpanHz},
            {QStringLiteral("toneAtOffsetDb"),
             db(certmath::tonePowerNear(mono, o.referenceOffsetHz, fs,
                                        kReferenceSearchSpanHz))},
            {QStringLiteral("overallRmsDb"), db(rms(mono))},
            {QStringLiteral("frames"), static_cast<int>(mono.size())},
        };
    }
    slice->setMode(o.mode);

    record(QStringLiteral("rx-sidebands"),
           QStringLiteral("All four SSB-family modes against a known carrier"),
           perMode,
           QStringLiteral(
               "With the dial parked so a known carrier sits at the configured "
               "offset, the modes whose passband covers that side should recover "
               "the tone and the other two should not. USB and DIGU should agree "
               "with each other, LSB and DIGL likewise, and the two pairs should "
               "disagree — if all four recover it, the dial is probably not where "
               "this stage thinks it is."),
           QStringLiteral(
               "Interpretation needs a real carrier present. With no antenna or "
               "no signal at the reference frequency every mode reads the noise "
               "floor and this stage is inconclusive rather than passing"),
           QStringLiteral("docs/HERMES.md 15.3, 15.4"));
}

void RadioCertification::stagePassbandAfterModeChange(const Options& o)
{
    // SetRXAMode DISCARDS a passband applied before it, so a backend that pushes
    // the filter and then the mode ends up with a sticky window. Arriving at DIGU
    // from CW handed the decoder a ~500 Hz passband and it decoded nothing, with
    // the mode indicator reading correctly the whole time.
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    slice->setMode(QStringLiteral("CW"));
    spin(700);
    const int cwLow = slice->filterLow(), cwHigh = slice->filterHigh();
    slice->setMode(QStringLiteral("DIGU"));
    spin(700);
    const int digLow = slice->filterLow(), digHigh = slice->filterHigh();
    slice->setMode(o.mode);
    spin(500);

    QJsonObject m{
        {QStringLiteral("cwWidthHz"), cwHigh - cwLow},
        {QStringLiteral("diguWidthHz"), digHigh - digLow},
        {QStringLiteral("cwPassband"), QStringLiteral("%1..%2").arg(cwLow).arg(cwHigh)},
        {QStringLiteral("diguPassband"), QStringLiteral("%1..%2").arg(digLow).arg(digHigh)},
    };

    QString concern;
    if (digHigh - digLow <= cwHigh - cwLow)
        concern = QStringLiteral(
            "the passband did not widen moving from CW to DIGU — it looks sticky. "
            "A radio that owns its DSP gets no mode echo to heal this, so the "
            "backend must supply a per-mode default passband and apply it AFTER "
            "the mode");

    record(QStringLiteral("passband-after-mode-change"),
           QStringLiteral("The passband follows a mode change"),
           m,
           QStringLiteral(
               "CW then DIGU is the ordering that exposed this: the narrow window "
               "survived into a wide mode and the decoder saw nothing."),
           concern,
           QStringLiteral("docs/HERMES.md 15.7"));
}

void RadioCertification::stagePreconditions()
{
    const bool connected = m_radio && m_radio->isConnected();
    const bool canTx = m_radio && m_radio->backendCanTransmit();
    QJsonObject m{
        {QStringLiteral("connected"), connected},
        {QStringLiteral("family"), m_radio ? m_radio->family() : QString()},
        {QStringLiteral("canTransmit"), canTx},
        {QStringLiteral("hostModulation"),
         m_radio ? m_radio->transmitModel().hostModulation() : false},
        {QStringLiteral("micSelection"),
         m_radio ? m_radio->transmitModel().micSelection() : QString()},
        {QStringLiteral("txAudioStreaming"), m_audio && m_audio->isTxStreaming()},
    };

    QString concern;
    if (!connected)
        concern = QStringLiteral("not connected — nothing below this will mean anything");
    else if (!canTx)
        concern = QStringLiteral("transmit unavailable; the backend reports RX-only");
    else if (m_audio && !m_audio->isTxStreaming())
        concern = QStringLiteral(
            "TX audio capture is NOT running. On a host-modulating backend this "
            "silences the microphone AND the test tone, because the tone is "
            "injected inside the mic callback — the radio will key and transmit "
            "nothing");

    record(QStringLiteral("preconditions"),
           QStringLiteral("Backend, transmit gate and audio capture"),
           m,
           QStringLiteral("What the app believes about itself before any key."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1 defects 1 and 2"));
}

void RadioCertification::stageControlPlane(const Options& o)
{
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (slice) {
        slice->setMode(o.mode);
        slice->setFrequency(o.frequencyMhz);
    }
    spin(1500);

    const double readbackMhz = slice ? slice->frequency() : 0.0;
    const QString readbackMode = slice ? slice->mode() : QString();
    QJsonObject m{
        {QStringLiteral("requestedMhz"), o.frequencyMhz},
        {QStringLiteral("readbackMhz"), readbackMhz},
        {QStringLiteral("requestedMode"), o.mode},
        {QStringLiteral("readbackMode"), readbackMode},
    };

    QString concern;
    if (std::fabs(readbackMhz - o.frequencyMhz) > 1e-6)
        concern = QStringLiteral(
            "the slice did not take the requested frequency; everything after "
            "this is measuring an unknown frequency");
    else if (readbackMode.compare(o.mode, Qt::CaseInsensitive) != 0)
        concern = QStringLiteral("the slice did not take the requested mode");

    record(QStringLiteral("control-plane"),
           QStringLiteral("Frequency and mode readback"),
           m,
           QStringLiteral(
               "Readback only proves the MODEL agrees. A radio that reports no "
               "VFO cannot be asked what it is really tuned to, so the app is "
               "authoritative and anything it fails to push is inherited from "
               "the previous session — that is how a VFO once read 10 MHz while "
               "the radio transmitted on 14."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1 defect on connect-time state"));
}

void RadioCertification::stageDspLiveness(const Options& o)
{
    if (!m_audio)
        return;
    // A known tone through the REAL audio path — the same entry the microphone
    // and the TONE button use — so this measures the chain, not a shortcut.
    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }
    keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject meters = meterSnapshot();
    keyViaOperatorPath(false);
    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    spin(800);

    const double micPeak = meters.value(QStringLiteral("micPeakDbfs")).toDouble(-140.0);
    QString concern;
    if (micPeak <= -139.0)
        concern = QStringLiteral(
            "no audio reached the modulator. The chain is broken ABOVE the DSP: "
            "either capture is not running, or the TX audio callback is gated on "
            "something this backend never satisfies");

    record(QStringLiteral("dsp-liveness"),
           QStringLiteral("Audio reaches the modulator"),
           meters,
           QStringLiteral(
               "Mic peak is measured pre-ALC, so it reports the level actually "
               "arriving rather than the ALC's success."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1 defects 1 and 2"));
}

void RadioCertification::stageRf(const Options& o)
{
    if (!m_audio)
        return;
    const QJsonObject idle = meterSnapshot();

    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }
    const bool keyedOk = keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject keyed = meterSnapshot();
    // This stage transmits a tone, so its keyed window is evidence for the
    // meters phase's keyed-RF precondition in an `all` run. The carrier-
    // suppression stage deliberately keys into silence and is NOT instrumented
    // for the same reason: a window that is supposed to produce no RF must not
    // be counted as one that failed to.
    if (keyedOk)
        observeKeyedRf();
    keyViaOperatorPath(false);
    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    spin(1200);
    const QJsonObject after = meterSnapshot();

    // A missing or stale meter is not missing RF: meters are not yet validated
    // here, so conclusions from them are labelled meterDependent and worded about
    // the meter. swrStale and stale PA temperature are excluded, or a leftover
    // value would fake a healthy SWR path or a 0.0 temperature rise.
    const bool haveSwr = keyed.contains(QStringLiteral("swr"))
                      && !keyed.contains(QStringLiteral("swrStale"));
    const bool haveTemp = idle.contains(QStringLiteral("paTempC"))
                       && keyed.contains(QStringLiteral("paTempC"))
                       && !keyed.contains(QStringLiteral("paTempCStale"))
                       && !idle.contains(QStringLiteral("paTempCStale"));
    const double tempIdle = idle.value(QStringLiteral("paTempC")).toDouble();
    const double tempKeyed = keyed.value(QStringLiteral("paTempC")).toDouble();
    QJsonObject m{
        {QStringLiteral("idle"), idle},
        {QStringLiteral("keyed"), keyed},
        {QStringLiteral("afterUnkey"), after},
        {QStringLiteral("swrAvailable"), haveSwr},
        {QStringLiteral("paTempAvailable"), haveTemp},
    };
    if (haveTemp)
        m[QStringLiteral("paTempRiseC")] = tempKeyed - tempIdle;

    QString concern;
    bool meterDependent = false;
    if (!haveSwr) {
        concern = QStringLiteral(
            "METER-DEPENDENT: no SWR reading while keyed. This is 'no SWR "
            "reading', NOT 'no RF' — the two are only the same thing once the "
            "meters phase has validated this radio's telemetry. Run "
            "'radiocert meters' before concluding anything about the PA");
        meterDependent = true;
    } else if (!haveTemp) {
        concern = QStringLiteral(
            "METER-DEPENDENT: no PA temperature meter on this radio, so "
            "dissipation could not be checked. Every other reading here can be "
            "faked by a wiring error; this was the one that could not");
        meterDependent = true;
    } else if (tempKeyed - tempIdle < 0.2) {
        concern = QStringLiteral(
            "PA temperature did not rise. A wiring error can fake every other "
            "reading here; dissipation is the one that cannot be faked");
        meterDependent = true;
    }

    record(QStringLiteral("rf"),
           QStringLiteral("The radio actually produces RF"),
           m,
           QStringLiteral(
               "SWR is meaningful uncalibrated because it is a ratio from one "
               "converter. Absolute power is NOT reported: raw counts dressed up "
               "as watts would be a confident lie."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1 defects 3 and 4"),
           meterDependent);
}

void RadioCertification::stageSideband(const Options& o)
{
    if (!m_audio || !m_radio || !o.includeAudioProbe)
        return;

    // Sideband check: demodulate our own transmission. A 1 kHz USB tone received
    // in USB must return 1 kHz; an inverted TX lands in LSB instead. The
    // panadapter can't answer this (raw wire order agrees with the transmitter by
    // construction); the demodulator's RX conjugation and WDSP sideband selection
    // are an independent path.
    m_radio->setTxAudioMonitor(true);   // hear ourselves, just for this stage

    // Drop drive for this stage: our own TX at full power overloads the receiver
    // (measured +7.3 dBFS, both sidebands alike). RAII restores power and the TX
    // monitor on every exit; run()'s epilogue does not restore power.
    auto& tx = m_radio->transmitModel();
    const int restorePower = tx.rfPower();
    const auto restore = qScopeGuard([this, restorePower] {
        // Re-check the pointers rather than capturing tx by reference: the whole
        // reason this stage can unwind early is a teardown mid-run, which is
        // also what would leave m_radio dangling.
        if (m_radio) {
            m_radio->transmitModel().setRfPower(restorePower);
            m_radio->setTxAudioMonitor(false);
            keyViaOperatorPath(false);
        }
        if (m_audio) {
            if (auto* t = m_audio->clientTxTestTone())
                t->setEnabled(false);
        }
    });
    tx.setRfPower(kSidebandProbePowerPercent);
    spin(600);

    // Each capture reports the rate it ACTUALLY saw, in its own out-parameter.
    // See the block below the captures for why this is not a constant.
    double sameFs = 0.0, oppFs = 0.0;
    auto capture = [&](const QString& mode, double& fsOut) -> std::vector<float> {
        if (auto* slice = m_radio->slice(0))
            slice->setMode(mode);
        spin(900);
        m_audio->startAutomationAudioCapture(o.settleMs + 500,
                                             QStringList{QStringLiteral("output")});
        keyViaOperatorPath(true);
        spin(o.settleMs);
        keyViaOperatorPath(false);
        m_audio->stopAutomationAudioCapture();
        spin(400);

        // Pull the captured PCM back out and flatten to mono.
        const QJsonObject snap = m_audio->automationAudioCaptureSnapshot(true);
        std::vector<float> mono;
        for (const QJsonValue& cv : snap.value(QStringLiteral("chunks")).toArray()) {
            const QJsonObject c = cv.toObject();
            const int ch = std::max(1, c.value(QStringLiteral("channels")).toInt(1));
            fsOut = c.value(QStringLiteral("sampleRate")).toDouble(fsOut);
            const QByteArray pcm = QByteArray::fromBase64(
                c.value(QStringLiteral("pcmBase64")).toString().toLatin1());
            const auto* f = reinterpret_cast<const float*>(pcm.constData());
            const int frames = static_cast<int>(pcm.size() / sizeof(float)) / ch;
            for (int n = 0; n < frames; ++n)
                mono.push_back(f[n * ch]);
        }
        return mono;
    };

    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }

    const std::vector<float> sameSb = capture(o.mode, sameFs);
    const QString oppositeMode = o.mode.compare(QStringLiteral("LSB"),
                                                Qt::CaseInsensitive) == 0
                                     ? QStringLiteral("USB") : QStringLiteral("LSB");
    const std::vector<float> oppSb = capture(oppositeMode, oppFs);

    // Power, monitor, key and tone are all handled by the scope guard above.
    if (auto* slice = m_radio->slice(0))
        slice->setMode(o.mode);

    // Take the rate from the capture (docs/HERMES.md 1.9): the "output" tap runs at
    // the audio device rate, not AudioEngine::DEFAULT_SAMPLE_RATE, and a wrong
    // rate probes the wrong frequency (1 kHz at an assumed 24 kHz on a 48 kHz
    // device reads 2 kHz). The rate is reported, and differing rates between the
    // two captures are a concern in their own right.
    const double fs = sameFs > 0.0 ? sameFs
                    : (oppFs > 0.0 ? oppFs : AudioEngine::DEFAULT_SAMPLE_RATE);
    const bool rateAssumed  = sameFs <= 0.0 && oppFs <= 0.0;
    const bool rateMismatch = sameFs > 0.0 && oppFs > 0.0
                           && std::fabs(sameFs - oppFs) > 1.0;

    const double sameTone = tonePower(sameSb, 1000.0, sameFs > 0.0 ? sameFs : fs);
    const double oppTone  = tonePower(oppSb, 1000.0, oppFs > 0.0 ? oppFs : fs);
    const double sameAll  = rms(sameSb);
    const double oppAll   = rms(oppSb);

    const bool saturated = db(std::max(sameAll, oppAll)) > -3.0;

    // THE DISCRIMINATOR IS ABSOLUTE LEVEL IN EACH LEG, NOT THE DIFFERENCE.
    //
    // See the observation text below for the full reasoning. In short: both legs
    // are matched TX/RX pairs, so a correct transmitter recovers the tone in
    // BOTH and an inverted one recovers it in NEITHER. Comparing the two legs
    // against each other measured noise; comparing each against the floor
    // measures whether the transmitter and the demodulator agree.
    const bool recoveredMatched  = db(sameTone) > kRecoveredFloorDb;
    const bool recoveredOpposite = db(oppTone) > kRecoveredFloorDb;

    QJsonObject m{
        {QStringLiteral("transmitMode"), o.mode},
        {QStringLiteral("sampleRateHz"), fs},
        {QStringLiteral("sampleRateAssumed"), rateAssumed},
        {QStringLiteral("recoveredToneFloorDb"), kRecoveredFloorDb},
        {QStringLiteral("recoveredMatched"), recoveredMatched},
        {QStringLiteral("recoveredOpposite"), recoveredOpposite},
        {QStringLiteral("txRxSidebandAgree"), recoveredMatched && recoveredOpposite},
        // Retained as evidence, but NOT a verdict input — see the observation.
        {QStringLiteral("legsAreMatchedPairsNotOpposites"), true},
        {QStringLiteral("demodMatched"), QJsonObject{
            {QStringLiteral("mode"), o.mode},
            {QStringLiteral("sampleRateHz"), sameFs},
            {QStringLiteral("tone1kDb"), db(sameTone)},
            {QStringLiteral("overallRmsDb"), db(sameAll)},
            {QStringLiteral("frames"), static_cast<int>(sameSb.size())}}},
        {QStringLiteral("demodOpposite"), QJsonObject{
            {QStringLiteral("mode"), oppositeMode},
            {QStringLiteral("sampleRateHz"), oppFs},
            {QStringLiteral("tone1kDb"), db(oppTone)},
            {QStringLiteral("overallRmsDb"), db(oppAll)},
            {QStringLiteral("frames"), static_cast<int>(oppSb.size())}}},
        {QStringLiteral("matchedMinusOppositeDb"), db(sameTone) - db(oppTone)},
        {QStringLiteral("receiverSaturated"), saturated},
    };

    // Saturation check first: our own TX over a few inches of coax overloads the
    // receiver (measured +7.3 dBFS) and makes the sidebands read alike. An
    // overloaded measurement must decline to answer, not report inversion.
    QString concern;
    if (sameSb.empty() || oppSb.empty()) {
        concern = QStringLiteral(
            "no receive audio captured while transmitting. The TX audio monitor "
            "may not be implemented for this backend, in which case this stage "
            "cannot run and the sideband must be checked against a second receiver");
    } else if (rateMismatch) {
        concern = QStringLiteral(
            "INCONCLUSIVE — the two captures reported different sample rates "
            "(%1 Hz and %2 Hz). Two buffers measured at different rates are not a "
            "sideband comparison, so no verdict can be drawn")
            .arg(sameFs, 0, 'f', 0).arg(oppFs, 0, 'f', 0);
    } else if (rateAssumed) {
        concern = QStringLiteral(
            "INCONCLUSIVE — no capture reported a sample rate, so the correlator "
            "fell back to the assumed %1 Hz. An assumed rate is the docs/HERMES.md 1.9 "
            "defect: the probe lands on the wrong frequency and both sidebands "
            "read the noise floor, which looks exactly like an inverted sideband")
            .arg(fs, 0, 'f', 0);
    } else if (saturated) {
        concern = QStringLiteral(
            "INCONCLUSIVE — the receiver is still saturated by our own "
            "transmission (overall RMS at or above -3 dBFS) even with the drive "
            "reduced for this stage. Both sidebands read alike when the front end "
            "is overloaded, so no verdict can be drawn. Lower "
            "kSidebandProbePowerPercent further, add attenuation, or check the "
            "sideband against a second receiver");
    } else if (!recoveredMatched && !recoveredOpposite) {
        concern = QStringLiteral(
            "TRANSMIT AND RECEIVE DISAGREE ABOUT SIDEBAND. Neither leg recovered "
            "the tone (%1 dB and %2 dB, floor at %3 dB). Because setting the "
            "slice mode moves BOTH chains together, a leg goes silent only when "
            "the transmitter puts the tone on the side the demodulator is not "
            "listening to — which is what a missing conjugation of the transmit "
            "IQ looks like (docs/HERMES.md 14.6)")
            .arg(db(sameTone), 0, 'f', 1).arg(db(oppTone), 0, 'f', 1)
            .arg(kRecoveredFloorDb, 0, 'f', 0);
    } else if (recoveredMatched != recoveredOpposite) {
        concern = QStringLiteral(
            "ASYMMETRIC — one sideband recovered the tone and the other did not "
            "(%1: %2 dB, %3: %4 dB). Both should behave alike when TX and RX move "
            "together. Suspect a mode whose transmit passband is not the mirror "
            "of its receive passband")
            .arg(o.mode).arg(db(sameTone), 0, 'f', 1)
            .arg(oppositeMode).arg(db(oppTone), 0, 'f', 1);
    }

    record(QStringLiteral("sideband"),
           QStringLiteral("Transmit/receive sideband AGREEMENT — not absolute correctness"),
           m,
           QStringLiteral(
               "WHAT THIS CAN AND CANNOT SEE. Setting the slice mode drives the "
               "transmit chain and the receive chain together (Hl2Backend::"
               "setSliceMode: 'the transmit sideband follows the slice'), so both "
               "legs here are matched pairs — TX-USB/RX-USB and TX-LSB/RX-LSB — "
               "and the older matched-versus-opposite COMPARISON could not "
               "discriminate anything: with the transmitter correct both legs "
               "recover the tone, and with it inverted both go silent. The "
               "difference between them was noise either way.\n\n"
               "What IS observable is the absolute level in both legs, and it "
               "answers a real question: do the transmitter and the demodulator "
               "agree about which side of the carrier a sideband is on. A "
               "transmit-only inversion silences both.\n\n"
               "A SHARED inversion — both chains wrong in the same direction — is "
               "invisible here BY CONSTRUCTION, which is docs/HERMES.md 1.1 recurring "
               "one level up: this check was built to catch convention errors and "
               "still shares a convention with its subject. Only an unrelated "
               "receiver settles it, and that check is listed in manualChecks."),
           concern,
           QStringLiteral("docs/HERMES.md 14.6; 1.1 — a shared convention cannot self-check"));
}

void RadioCertification::stageCarrierSuppression(const Options& o)
{
    if (!m_audio)
        return;
    // SSB has no carrier: keying with no audio should give ~no RF; anything
    // substantial is modulator DC offset. Mic gain goes to zero because the ALC
    // lifts room noise into real modulation otherwise. AudioEngine has no getter,
    // so restore from the persisted "PcMicGain" setting MainWindow applies.
    const int restoreGain = AppSettings::instance().value("PcMicGain", 100).toInt();
    if (m_audio)
        m_audio->setPcMicGain(0);
    spin(600);
    keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject keyedSilent = meterSnapshot();
    keyViaOperatorPath(false);
    if (m_audio)
        m_audio->setPcMicGain(restoreGain);
    spin(800);

    QJsonObject m{{QStringLiteral("keyedWithNoAudio"), keyedSilent},
                  {QStringLiteral("micGainDuringTest"), 0},
                  {QStringLiteral("micGainRestoredTo"), restoreGain}};
    QString concern;
    const bool freshSwr = keyedSilent.contains(QStringLiteral("swr"))
                       && !keyedSilent.contains(QStringLiteral("swrStale"));
    if (freshSwr)
        concern = QStringLiteral(
            "the radio reported forward power while keyed with NO audio. In SSB "
            "that is a carrier — most likely a DC offset in the modulator, or "
            "audio left over from a previous transmission that was not flushed");

    record(QStringLiteral("carrier-suppression"),
           QStringLiteral("Keyed with no audio produces no carrier"),
           m,
           QStringLiteral(
               "SSB should be silent when the operator is. A reading here is "
               "either a DC-offset carrier or an unflushed transmit queue."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1; queue flush on unkey"));
}

void RadioCertification::stageLifecycle(const Options&)
{
    if (!m_audio || !m_radio)
        return;

    // Receive audio must be silent DURING transmit and must not burst afterwards.
    // Muting only the output is not enough: the demodulator keeps running on our
    // own signal, its filters fill, and the backlog drains on unkey — heard as
    // the tail of a transmission playing back after it ended.
    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject before = m_audio->automationAudioCaptureSnapshot(false);

    keyViaOperatorPath(true);
    spin(600);
    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject during = m_audio->automationAudioCaptureSnapshot(false);
    keyViaOperatorPath(false);

    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject afterUnkey = m_audio->automationAudioCaptureSnapshot(false);

    const auto bytes = [](const QJsonObject& o) {
        return o.value(QStringLiteral("capturedBytes")).toInt();
    };
    QJsonObject m{
        {QStringLiteral("rxBytesBeforeKey"), bytes(before)},
        {QStringLiteral("rxBytesDuringTx"), bytes(during)},
        {QStringLiteral("rxBytesAfterUnkey"), bytes(afterUnkey)},
    };

    QString concern;
    if (bytes(during) > 0)
        concern = QStringLiteral(
            "receive audio was still flowing during transmit. The operator will "
            "hear their own carrier as fuzz and their own voice as feedback, and "
            "with an open microphone that closes an acoustic loop that corrupts "
            "the transmitted audio");
    else if (bytes(afterUnkey) == 0 && bytes(before) > 0)
        concern = QStringLiteral(
            "receive audio did not resume after unkey");

    record(QStringLiteral("lifecycle"),
           QStringLiteral("Receive muting across the transmit cycle"),
           m,
           QStringLiteral(
               "Zero bytes during transmit is what is wanted. A non-zero count "
               "AFTER unkey that greatly exceeds the before-key rate would "
               "indicate a drained backlog rather than a resumed stream."),
           concern,
           QStringLiteral("docs/HERMES.md 14.1; RX mute at the demodulator"));
}

QJsonObject RadioCertification::run(const Options& o)
{
    m_stages = QJsonArray{};
    // Per-run evidence, cleared with the stages it is reported beside. Carrying
    // a previous run's keyed-RF confirmation forward would make the second run
    // of a session inherit the first one's transmission.
    m_keyedFwdWattsMax = -1.0;
    m_keyedRfSamples = 0;
    m_keyedWindows = 0;
    m_fwdPowerMeterDefined = false;
    m_renderedWhileKeyed = QJsonObject{};
    m_keyRefusals = 0;

    // LEAVE THE RADIO WHERE WE FOUND IT.
    //
    // Every stage that moves the dial restored it to o.frequencyMhz (14.200 by
    // default), not to where the operator actually had it — so `radiocert tune`,
    // the one phase safe enough to need no TX permission, silently relocated the
    // operator's VFO to 20 m and left it there. Mic gain and drive level were
    // both carefully saved and restored, which is what made this an oversight
    // rather than a decision.
    double operatorMhz = 0.0;
    QString operatorMode;
    if (m_radio) {
        if (auto* slice = m_radio->slice(0)) {
            operatorMhz = slice->frequency();
            operatorMode = slice->mode();
        }
    }

    // SILENCE QUINDAR FOR THE RUN.
    //
    // Two separate problems, one cause. The outro tone defers the real unkey
    // behind a QTimer, so "unkeyed" and "requestPttOff returned" are different
    // moments; and the intro tone is transmitted as audio, which lands straight
    // inside stage-carrier-suppression's assertion that nothing is being sent.
    // A diagnostic cannot measure a chain that is injecting its own audio into
    // it (docs/HERMES.md 1.12 — "no audio" has to mean it).
    bool quindarWas = false;
    ClientQuindarTone* quindar = m_audio ? m_audio->clientQuindarTone() : nullptr;
    if (quindar) {
        quindarWas = quindar->isEnabled();
        quindar->setEnabled(false);
    }

    // CLAMP RF POWER FOR THE WHOLE RUN, before any stage can key.
    //
    // The bridge's power ceiling is applied where a widget setpoint is written,
    // and this verb keys through its own path — so every keyed stage ran at
    // whatever RF power the operator had set, on the one verb that keys
    // repeatedly and unattended. That is the case the ceiling exists for.
    int powerToRestore = -1;
    if (m_radio && o.maxRfPowerPercent >= 0) {
        auto& tx = m_radio->transmitModel();
        const int current = tx.rfPower();
        if (current > o.maxRfPowerPercent) {
            powerToRestore = current;
            tx.setRfPower(o.maxRfPowerPercent);
            qCInfo(lcAutomation).noquote()
                << "radiocert: RF power clamped" << current << "->"
                << o.maxRfPowerPercent << "for the run (automation ceiling)";
        }
    }

    // EVERYTHING ABOVE IS RESTORED BY THIS ONE GUARD, INCLUDING THE UNKEY.
    //
    // The unkey/tone/monitor half used to be straight-line code at the end of
    // run(). There is no reachable path past it today, but the failure mode if
    // one were ever added is uniquely bad on this verb: an early return would
    // leave the radio KEYED, and the same missed epilogue would leave the
    // watchdog already disowned by onTxWatchdog() — so the backstop fails in the
    // same instant the bug is introduced. Cheap insurance on the one verb whose
    // failure mode is a stuck transmitter.
    const auto epilogue = qScopeGuard([&] {
        keyViaOperatorPath(false);
        if (m_audio) {
            if (auto* tone = m_audio->clientTxTestTone())
                tone->setEnabled(false);
        }
        if (quindar)
            quindar->setEnabled(quindarWas);
        if (m_radio) {
            m_radio->setTxAudioMonitor(false);
            if (powerToRestore >= 0)
                m_radio->transmitModel().setRfPower(powerToRestore);
            // ...and back on the operator's frequency and mode, not ours.
            if (auto* slice = m_radio->slice(0)) {
                if (operatorMhz > 0.0)
                    slice->setFrequency(operatorMhz);
                if (!operatorMode.isEmpty())
                    slice->setMode(operatorMode);
            }
        }
    });

    const bool all    = o.phase == Phase::All;
    const bool doTune = all || o.phase == Phase::Tune;
    const bool doRx   = all || o.phase == Phase::Rx;
    const bool doTx   = all || o.phase == Phase::Tx;
    const bool doMet  = all || o.phase == Phase::Meters;

    // RECEIVE FIRST WHEN BOTH ARE SELECTED, and not for tidiness. The wire's
    // handedness is ONE fact that both directions consume, and transmit cannot
    // be reasoned about until it is settled — a transmit result read before the
    // receive convention is known is a result about an unknown quantity.
    if (doTune) {
        stageModeMap();
        stageTuning(o);
        // In an `all` run the transmit block re-runs this, because by then the
        // receive stages have moved the dial. Running it twice would file two
        // stages with the same id, so tune-only owns it when tx is not selected.
        if (!doTx)
            stageControlPlane(o);
    }

    if (doRx) {
        stageZeroShift(o);
        stageRxSidebands(o);
        stageConsumerAgreement(o);
        stagePassbandAfterModeChange(o);
    }

    if (doTx) {
        // TX safety: re-establish the dial before anything keys. The RX stages park it
        // on the WWV reference carrier and restore only the mode, so keyed stages
        // would otherwise transmit out of band (docs/HERMES.md 1.11). Also gives
        // standalone `radiocert tx` a known frequency.
        stageControlPlane(o);
        stagePreconditions();
        stageDspLiveness(o);
        stageRf(o);
        stageCarrierSuppression(o);
        stageSideband(o);
        stageLifecycle(o);
    }

    if (doMet) {
        // NON-KEYING INSTRUMENTS FIRST. They need no TX dial; the squelch stage
        // parks the dial on its carrier and puts it back before anything keys.
        stageControlDomain(o);
        stageFrontEndInterlock();
        stageSquelchScale(o);

        // THIS BLOCK KEYS TOO, so it needs the same dial guarantee the transmit
        // block above established.
        //
        // stageMeterScale() and stageControlEffect() both transmit. Without this,
        // `radiocert meters` standalone keyed on whatever the operator last tuned
        // — the identical defect fixed for the transmit block, left in place for
        // this one because the keying here is less obvious from the phase name.
        // In an `all` run doTx has already done it, and re-running would file two
        // stages under one id, so this mirrors doTune's guard.
        if (!doTx) {
            stageControlPlane(o);
            stagePreconditions();
        }
        // INVENTORY LAST. A transmit meter cannot have a value before anything
        // has transmitted, so running the inventory first reports every TX meter
        // as "never fed" — true, and useless. Exercise them, then take stock:
        // what is still unfed after a keyed stage is genuinely unfed.
        stageMeterScale(o);
        stageControlEffect(o);
        stageMeterInventory();
    }

    // Unkey, restore and re-tune all happen in `epilogue` above.

    // What this instrument CANNOT determine, stated as work for a human rather
    // than omitted. Everything here needs either a second receiver, an ear, or
    // equipment this application does not have — and pretending otherwise is
    // exactly how a wrong-sideband transmitter passed every check it had.
    QJsonArray manual{
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Sideband against an unrelated receiver")},
            {QStringLiteral("why"), QStringLiteral(
                "The sideband stage above demodulates our own transmission, which "
                "is a different path from the panadapter but still our own code. "
                "Two errors in the same direction would agree. Tune a separate "
                "radio to the same frequency and confirm the mode matches.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Audio quality and intelligibility")},
            {QStringLiteral("why"), QStringLiteral(
                "Level and frequency can both be correct while the audio is "
                "clipped, aliased or unintelligible. Nothing here measures "
                "distortion. Listen on another receiver.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Occupied bandwidth and splatter")},
            {QStringLiteral("why"), QStringLiteral(
                "A modulator can hit 85 dB opposite-sideband suppression and "
                "still radiate outside its passband — that happened here, and "
                "only a deliberate out-of-band probe found it. Check the signal "
                "width on a second receiver's panadapter.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Harmonics and spurs")},
            {QStringLiteral("why"), QStringLiteral(
                "The receive window is a few tens of kHz wide and centred on the "
                "transmit frequency, so it cannot see a harmonic by construction.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("ALC behaviour on real speech")},
            {QStringLiteral("why"), QStringLiteral(
                "A steady tone cannot reveal pumping between words or a first "
                "syllable lost to the attack. Speak, and listen.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Long-transmission thermal behaviour")},
            {QStringLiteral("why"), QStringLiteral(
                "These stages key for a few seconds each. A net-length "
                "transmission is a different question for the PA.")}},
    };

    if (doRx) {
        manual.append(QJsonObject{
            {QStringLiteral("check"), QStringLiteral(
                "Panadapter versus demodulator, with a signal OFF centre")},
            {QStringLiteral("why"), QStringLiteral(
                "They are independent consumers of the same buffer and can "
                "disagree; when they did, each had the other's convention. A "
                "mirror is invisible on the pan centre, so the signal must be "
                "off-axis. This is the check that found the receive inversion, "
                "and it is not yet automated.")}});
        manual.append(QJsonObject{
            {QStringLiteral("check"), QStringLiteral(
                "Spots from a third party in the mode under test")},
            {QStringLiteral("why"), QStringLiteral(
                "PSK Reporter or a cluster spot is evidence that cannot come "
                "from a self-consistent loop. The receive bring-up ended with 63 "
                "spots on 14.074 DIGU — the first proof that was not our own "
                "code agreeing with itself.")}});
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("kind"), QStringLiteral("radio-bringup-diagnostic")},
        {QStringLiteral("phase"),
             o.phase == Phase::Tune   ? QStringLiteral("tune")
           : o.phase == Phase::Rx     ? QStringLiteral("rx")
           : o.phase == Phase::Tx     ? QStringLiteral("tx")
           : o.phase == Phase::Meters ? QStringLiteral("meters")
                                      : QStringLiteral("all")},
        {QStringLiteral("note"), QStringLiteral(
            "Diagnostic only — this deliberately does not pass or fail. Read the "
            "concerns, then the measurements.")},
        {QStringLiteral("radio"), QJsonObject{
            {QStringLiteral("family"), m_radio ? m_radio->family() : QString()},
            // The dial the KEYED stages ran on. Reported because an earlier
            // version left the receive stages' reference carrier in place and
            // this field went on claiming the requested frequency regardless.
            {QStringLiteral("frequencyMhz"), o.frequencyMhz},
            {QStringLiteral("mode"), o.mode},
            {QStringLiteral("txPowerCeilingPercent"), o.maxPowerPercent}}},
        // KEYS THE RADIO REFUSED. runPttPreflight can block a key (band limits,
        // interlocks) and requestPttOn returns void, so a refusal is otherwise
        // silent and every stage below reports its own subject as broken. A
        // non-zero count here invalidates the transmit stages rather than
        // merely annotating them.
        {QStringLiteral("keyRefusals"), m_keyRefusals},
        // DID ANY OF IT ACTUALLY RADIATE. Reported at the top level, not only
        // inside the meters phase, because a run that keyed cleanly and put out
        // no RF invalidates every transmit measurement in it in exactly the way
        // keyRefusals above invalidates a run the radio declined to key — and
        // that case is harder to notice, since nothing refused anything.
        {QStringLiteral("keyedRf"), QJsonObject{
            {QStringLiteral("confirmed"), keyedRfConfirmed()},
            {QStringLiteral("keyedWindows"), m_keyedWindows},
            {QStringLiteral("maxWattsWhileKeyed"),
             m_keyedRfSamples > 0 ? QJsonValue(m_keyedFwdWattsMax) : QJsonValue()},
            {QStringLiteral("floorWatts"), static_cast<double>(kKeyedRfFloorWatts)},
            // Asked of the model, not of the observation flag — see the same
            // field in stageMeterInventory for why the two differ on a run
            // where no keyed window ever looked.
            {QStringLiteral("forwardPowerMeterDefined"),
             m_radio && m_radio->meterModel().findMeter(
                 QStringLiteral("TX"), QStringLiteral("FWDPWR")) >= 0}}},
        {QStringLiteral("stages"), m_stages},
        {QStringLiteral("manualChecks"), manual},
    };
}

}  // namespace AetherSDR
