#include "core/RadioCertification.h"

#include "core/RadioCertificationMath.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientQuindarTone.h"
#include "core/LogManager.h"
#include "core/ClientTxTestTone.h"
#include "core/MeterSurfaces.h"
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
#include <optional>
#include <vector>

namespace AetherSDR {

using certmath::db;
using certmath::rms;
using certmath::tonePower;


namespace {

// The meter table from docs/radio-certification.md, in executable form.
//
// Kept as DATA rather than a sequence of hand-written checks so that adding a
// radio means adding rows, and so the documentation and the tool cannot drift
// apart — anything here that a backend does not publish shows up as "defined
// but never fed" or as absent, which are different findings and both useful.
//
// THERE IS NO UNIT COLUMN HERE, deliberately. It used to hold one expected unit
// compared by equality, and MeterSurfaces.h's `acceptedUnits` — a SET — was
// introduced precisely because that form reports a permanent false positive on
// a healthy meter. Only one of the two tables was updated, so every HL2 run
// went on reporting `UNIT MISMATCH … TX:ALC (declared dBFS, expected dB)` on a
// correct meter, and ranked it above every real concern (CERTIFICATION.md
// 1.38). The unit expectation now has exactly one home: kMeterSurfaces, joined
// by key. A row here whose key has no surface entry gets no unit verdict, which
// is the honest answer rather than a comparison against a value nobody wrote.
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
    {"TX",  "COMPPEAK", true,  true,  false, "host speech processor, polled onto the "
                                             "meter at 20 Hz; reads 0 with PROC off, "
                                             "which is a value and not a silence"},
    {"RAD", "PATEMP",   true,  false, false, "rise under key is the check, not the value"},
    {"RAD", "+13.8A",   false, false, false, "no supply telemetry on this radio"},
};

}  // namespace

RadioCertification::RadioCertification(RadioModel* radio, AudioEngine* audio)
    : m_radio(radio), m_audio(audio) {}

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

void RadioCertification::setKeyObserver(std::function<void(bool)> observer)
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
    // Tell the observer BEFORE keying and AFTER the radio has ACTUALLY unkeyed,
    // so the caller's safety window always encloses the transmission rather than
    // trailing it.
    if (on && m_onKey)
        m_onKey(true);

    auto& tx = m_radio->transmitModel();
    if (on) {
        tx.requestPttOn(TransmitModel::PttSource::Mox);

        // CONFIRM THE KEY REACHED THE RADIO. requestPttOn returns void and
        // silently does nothing when runPttPreflight() refuses — a band-limit
        // block, a missing antenna, an interlock. Every stage below then
        // measures an unkeyed radio and reports its silence as a defect in
        // whatever it happens to be testing: "audio never reached the
        // modulator", "the transmitter is not producing RF". The diagnostic
        // would blame the chain for a refusal it never noticed.
        spin(250);
        if (!keyedNow()) {
            ++m_keyRefusals;
            if (m_onKey)
                m_onKey(false);
            return false;
        }
        return true;
    }

    tx.requestPttOff(TransmitModel::PttSource::Mox);

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
        m_onKey(false);
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

    // NOTE: this stage does not exercise the RF power slider, and must not claim
    // to. It previously reported `rfPowerRestoredTo`, which named a restore that
    // never happened — in a report whose entire value is that it does not
    // overstate what it verified. The power row in docs/radio-certification.md
    // used to be certified by TX:FWDPWR dropping ~6 dB on a halving. FWDPWR IS
    // published on this backend now (in dBm, through an uncalibrated reference
    // curve), so that row is no longer blocked on a missing meter — but the
    // HALVING STIMULUS ITSELF IS NOT USABLE ON THIS RADIO, and the ratio does
    // not rescue it. Two independent reasons, both measured:
    //
    //   * The gateware decodes only the drive register's TOP NIBBLE, so a
    //     slider halving is not a drive halving. Slider 44 % and 50 % both land
    //     on nibble 7 (1.984 W vs 2.001 W — six points of travel doing
    //     nothing), and 51 % jumps +1.25 dB. 100→50→25 % is nibble 15→7→3.
    //   * HL2FilterE3 is nonlinear as well as uncalibrated, so the scale does
    //     not cancel out of a ratio taken across a wide span.
    //
    // Measured live: −4.44 dB (100→50 %) and −2.33 dB (50→25 %) against the
    // −6.02 dB a true halving would give. If the scale cancelled, both would
    // read −6.02. So a failing delta cannot be attributed to the control rather
    // than to the curve, and radiocert must not report one as a control defect.
    // What this control CAN certify by effect is monotonicity — one nibble up,
    // FWDPWR rises — which is the stimulus docs/radio-certification.md now
    // carries. See docs/HERMES.md 17.5 and 17.7 for both measurements.
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

    // RF GAIN, CERTIFIED BY EFFECT — AND THE FAMILY GATE GOES WITH IT.
    //
    // This used to hardcode, behind `if (family == "hl2")`, the finding
    // "SliceModel::setRfGain has no runtime path on this backend". The first
    // clause is still true: SliceModel::setRfGain's whole body is
    // `slice set N rfgain=X`, Flex wire text no seam backend can receive. The
    // CONCLUSION was false, and printed on every Hermes-Lite 2 run. The
    // operator's slider does not call it — SpectrumOverlayMenu emits
    // rfGainChanged unconditionally and only falls back to the slice setter
    // when there is no radio model or no pan id — so on the HL2 the slider
    // routes rfGainChanged → RadioModel::setPanRfGainFor →
    // Hl2Backend::setPanRfGain → applyLnaGainDb → MetisClient::setLnaGainDb,
    // which writes AD9866 0x0a[5:0] at runtime and remembers it per band.
    //
    // Replacing the assertion with a measurement removes the reason for the
    // family gate (§1.14). A stage that DRIVES the control and reports what
    // happened cannot tell a Flex operator their working preamp is broken,
    // because it is no longer telling anybody anything it did not observe.
    //
    // THE EXPECTED DELTA IS ZERO, NOT THE STEP SIZE. Hl2DbReference is moved in
    // the same call as the gain and both the spectrum and the S-meter render
    // through it, specifically so a gain change does NOT slide the display
    // (docs/HERMES.md 17.4; the class header states it as an invariant — "a gain
    // change provably cannot move a reported dBm value"). On healthy hardware
    // an 8 dB LNA step moves SLC:LEVEL by 0 dB. Asserting 8 dB would have
    // replaced one permanent false positive with another.
    //
    // AND THE DELTA STILL DOES NOT RENDER A VERDICT, because it cannot. Two
    // different states produce the identical −step reading:
    //
    //   * the reference moved and the LNA register did not — the real defect;
    //   * the register moved and the RECEIVED NOISE FLOOR did not, because the
    //     reading is dominated by the converter's own noise rather than by
    //     anything coming down the feedline. Raising the LNA lifts antenna
    //     noise and leaves ADC noise where it is, so on a quiet band the raw
    //     dBFS barely moves and the display subtracts the full step anyway.
    //
    // That is not a hypothetical. Measured on the live Hermes-Lite 2 on a quiet
    // 20 m: an 8 dB step moved SLC:LEVEL by −6.3 dB, which is within 2 dB of
    // the defect signature — while the raw dBFS behind it ROSE 1.74 dB, proving
    // the register HAD been written. A threshold tight enough to catch the
    // defect fires on healthy hardware every time the band is quiet, and this
    // stage exists to stop printing findings like that.
    //
    // So the delta is reported as evidence with its two readings named, and the
    // VERDICT rests on the echo. applyLnaGainDb echoes the value the hardware
    // actually took to every pan, so an echo landing on
    // PanadapterModel::rfGain() is evidence the seam was crossed — and it is
    // not §1.6's readback of our own setpoint, because an out-of-range request
    // comes back clamped rather than repeated. What is still missing to close
    // the effect half is the raw pre-reference dBFS, which the seam does not
    // expose (docs/CERTIFICATION.md 2.4 — the meters join).
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

        const double before = settledSLevel();
        m_radio->setPanRfGainFor(panId, target);
        const double after = settledSLevel();
        const int echoed = pan->rfGain();
        m_radio->setPanRfGainFor(panId, startGain);   // leave it where we found it
        spin(400);

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
        // THE UNIT, TWICE. `acceptedUnits` is every unit the CONSUMER can
        // correctly handle; `declaredUnit` is what the backend actually
        // published. When the declaration is outside the set the meter is
        // already being misread and no amount of freshness will show it — an
        // IC-705 declaring FWDPWR in Watts against a consumer assuming dBm
        // rendered 5 W as 0.003 W while this stage reported it fed and healthy.
        //
        // The set comes from kMeterSurfaces, the one table that owns it, and
        // the membership test from the one predicate that answers it. This used
        // to be a private single-value column compared by equality, which
        // flagged a correct TX:ALC on every HL2 run (CERTIFICATION.md 1.38).
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

    // WHAT THE GAUGE WILL ACTUALLY SHOW. Everything above measures the seam;
    // these three are read back through the consumer that converts, because
    // "a value arrived" and "the face moved" turned out to be different
    // questions (CERTIFICATION.md 1.27).
    //
    // TAKEN WHILE KEYED, by stageMeterScale, and behind each consumer's own
    // liveness gate. Sampled here instead — after stageControlEffect unkeyed
    // and settled 700 ms — every transmit quantity is absent by construction,
    // and read raw the SWR accessor returns its 1.0f initialiser, so the probe
    // reported a confident perfect match beside its own "never fed" (1.34).
    // The unkeyed reading is kept alongside because the pair is the evidence
    // that the gauge falls back correctly when transmission stops.
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
        if (back.compare(mode, Qt::CaseInsensitive) != 0)
            notRetained << (mode + QStringLiteral("->") + back);
    }
    if (slice && !restore.isEmpty())
        slice->setMode(restore);

    QString concern;
    if (!notRetained.isEmpty())
        concern = QStringLiteral(
            "these modes did not survive a round trip: ") + notRetained.join(", ")
            + QStringLiteral(". A mode the backend does not map does not fail — "
                             "it becomes the default, usually USB, while the UI "
                             "still shows what was asked for");

    record(QStringLiteral("mode-map"),
           QStringLiteral("Every mode the app can emit survives a round trip"),
           results,
           QStringLiteral(
               "READBACK IS NOT PROOF. The slice keeps whatever string it is "
               "given, so every mode can come back clean while the backend maps "
               "only some of them and silently demodulates the rest as its "
               "default. Confirmed on this radio: RTTY survives the round trip "
               "and the backend has no mapping for it. Compare each passband "
               "against the fallback mode's — an exact match is a hint, and the "
               "only one visible from outside the backend."),
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

    // ABSENCE OF A METER IS NOT ABSENCE OF RF.
    //
    // Both branches below used to read a missing meter as a transmit defect: an
    // absent SWR became "the transmitter is not producing RF", and an absent
    // PATEMP became a fabricated 0.0 rise and the accusation that dissipation
    // did not happen — on a radio that simply has no temperature telemetry.
    //
    // That is precisely the misattribution the meters-last ordering exists to
    // avoid, and this stage was committing it. The meters have NOT been
    // validated at this point in the run, so anything concluded from them is
    // labelled meterDependent and worded as a statement about the meter.
    // A STALE reading is not a reading. meterSnapshot() already records
    // swrStale, and without consulting it a value left over from a previous
    // transmission satisfies this check — so the stage would report a healthy
    // SWR path on a radio that sent nothing this key. That is the same
    // meter-trust problem the wording below is careful about, one level up.
    //
    // The PA temperature needs it for the same reason and did not have it: a
    // stale pair yields a fabricated 0.0 rise, and the stage then asserts that
    // dissipation did not happen on a radio it never actually read.
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

    // THE STAGE THIS WHOLE TOOL EXISTS FOR.
    //
    // Demodulate our own transmission and ask what FREQUENCY comes back. A 1 kHz
    // tone transmitted in USB and received in USB must return 1 kHz of audio. If
    // the transmit sideband is inverted it lands on the other side of the
    // carrier, the USB demodulator hears nothing, and the LSB demodulator hears
    // it instead.
    //
    // This works where the panadapter does not. The panadapter reads raw wire
    // order and so agrees with the transmitter no matter which convention it
    // uses; the demodulator applies the receive conjugation and WDSP's sideband
    // selection, which are an independent implementation of the same question.
    m_radio->setTxAudioMonitor(true);   // hear ourselves, just for this stage

    // TURN THE POWER DOWN FOR THIS STAGE.
    //
    // Listening to our own transmitter means the receiver sees full transmit
    // power from a few inches of coax and overloads — measured at +7.3 dBFS,
    // clipping hard, with both sidebands reading alike. A saturated front end
    // cannot answer a sideband question at all, so the stage was reduced to
    // reporting INCONCLUSIVE.
    //
    // This check needs enough signal to measure and no more. Drop the drive,
    // measure, restore.
    //
    // RAII, because the comment here used to CLAIM the setting was restored even
    // if something below threw while the code was plain sequential — an early
    // return or a throw would have left the operator's radio at 5% drive with the
    // TX audio monitor still on (RX unmuted during transmit), and nothing in the
    // UI to explain either. run()'s epilogue clears the monitor and the key but
    // has never restored the power.
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

    // TAKE THE RATE FROM THE CAPTURE. This is docs/HERMES.md 1.9, and it was still
    // here — in the one stage this whole tool exists for — after the same defect
    // had already been found and fixed in stage-rx-sidebands.
    //
    // The old comment said the rate was safe because "every chunk here comes
    // from the same sink". That was true and was exactly why it was wrong: the
    // sink is the "output" tap, which runs at the audio DEVICE's rate, not
    // AudioEngine::DEFAULT_SAMPLE_RATE. On a 48 kHz device, probing a 1 kHz tone
    // at an assumed 24 kHz lands on 2 kHz — both sidebands read the noise floor
    // and the verdict reduces to a coin toss that can print SIDEBAND LOOKS
    // INVERTED. The saturation guard below masked it, because rms() does not
    // care about the rate; the first person to add attenuation would have got a
    // confident wrong answer.
    //
    // The rate is reported in `measured` so a future wrong one is visible in the
    // output instead of silent, and a disagreement between the two captures is a
    // concern in its own right — comparing two buffers measured at different
    // rates is not a sideband comparison at all.
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

    // SATURATION CHECK FIRST, and this is not a detail.
    //
    // Monitoring our own transmission means the receiver sees a signal from a
    // few inches of coax at full transmit power. It overloads, both sidebands
    // read the same, and the sideband comparison becomes a coin toss on noise.
    //
    // Measured on hardware before this guard existed: overall RMS +7.3 dBFS —
    // clipping hard — with the two sidebands 1 dB apart, and the stage
    // confidently reported SIDEBAND LOOKS INVERTED. A false alarm of exactly the
    // kind that costs days, produced by the tool meant to prevent them. An
    // overloaded measurement must decline to answer, not answer wrongly.
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
    // SSB has no carrier. Keying with NO audio should produce essentially no RF;
    // anything substantial is DC offset in the modulator leaking a carrier at the
    // dial frequency.
    //
    // "NO AUDIO" HAS TO MEAN IT. The microphone is live during every other
    // stage, and the ALC's whole job is to lift a quiet room to full modulation
    // — so simply not enabling the test tone leaves room noise being amplified
    // into a real signal. Measured before this: mic peak -65 dBFS while
    // "silent", and the stage reported a carrier that was actually amplified
    // room ambience.
    //
    // Mic gain to zero is the honest way to ask the question.
    // AudioEngine exposes no getter, so restore from the persisted setting the
    // app itself uses — the same value MainWindow applies on connect.
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
        // RE-ESTABLISH THE DIAL BEFORE ANYTHING KEYS, and this is a transmit
        // safety property, not tidiness.
        //
        // stageControlPlane is the only thing that sets the frequency, and it
        // used to live in the tune block alone. The receive stages that run
        // between them park the dial on the reference carrier — WWV at
        // 9.9985 MHz — and restore the MODE but not the frequency. So every
        // keyed stage of an `all` run transmitted a few hundred Hz off a
        // standards station, out of band, while the report's `radio` block
        // faithfully said 14.2 MHz throughout: a value answering a different
        // question than the one being asked (docs/HERMES.md 1.11).
        //
        // It also fixes `radiocert tx` standalone, which otherwise keys on
        // whatever the operator last tuned, with no record of where that was.
        stageControlPlane(o);
        stagePreconditions();
        stageDspLiveness(o);
        stageRf(o);
        stageCarrierSuppression(o);
        stageSideband(o);
        stageLifecycle(o);
    }

    if (doMet) {
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
