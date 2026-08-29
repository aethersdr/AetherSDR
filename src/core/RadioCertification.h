#pragma once

#include "models/MeterModel.h"   // kMinForwardWattsForSwr — the keyed-RF floor

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <functional>

namespace AetherSDR {

class RadioModel;
class AudioEngine;

// Transmit bring-up instrument: drive the whole transmit chain against the
// radio itself and report what every stage actually did.
//
// THIS IS A DIAGNOSTIC, NOT A CERTIFICATION. It deliberately does not return
// pass/fail. Thresholds that are meaningful for one radio are guesses for the
// next, and a tool that says PASS on a radio nobody has characterised is worse
// than one that says "forward power 3846 counts, mic peak -32 dBFS, sideband
// consistent". Judgement stays with the operator or the agent reading it; this
// gathers the evidence they would otherwise gather by hand over several days.
//
// It assumes NO simulator and no second receiver — everything here runs against
// one radio, because that is the situation a new backend starts in.
//
// WHAT IT IS BUILT FROM. Every stage exists because something in the
// Hermes-Lite 2 bring-up failed silently at exactly that point. The stage list
// is a transcription of docs/HERMES.md section 14, and each result carries the
// reference so a future agent lands on the write-up rather than re-deriving it:
//
//   - four separate defects each produced a correct-looking keyed transmission
//     with ZERO forward power (14.1)
//   - a modulator with 85 dB opposite-sideband suppression radiated double
//     sideband, because the quadrature filter was all-pass in magnitude (14.2)
//   - keying reached the radio from the automation bridge and not from the MOX
//     button, because they drive different models (14.5)
//   - transmit ran on the WRONG SIDEBAND while every internal instrument agreed
//     it was right (14.6)
//
// The last one shapes the design. A convention error is invisible to any check
// that shares the convention, so the sideband stage demodulates our own
// transmission rather than looking at the panadapter: the panadapter reads raw
// wire order and agrees with the transmitter by construction, while the
// demodulator applies the receive conjugation and WDSP's sideband selection
// independently. That is a genuinely different path — though still not as
// strong as an unrelated receiver, which is why the report ends with the manual
// checks a human must still perform.
class RadioCertification {
public:
    // Bring-up order, and it is deliberate: each phase depends only on the ones
    // before it.
    //
    //   Tune    control plane only — no DSP, no audio, no meters
    //   Rx      demodulation and handedness — audio evidence, still no meters
    //   Tx      keying and modulation — audio evidence where it exists
    //   Meters  the instruments themselves, LAST, against known stimuli
    //
    // Meters come last because they are not trustworthy until something has
    // checked them, and the earlier phases must therefore not lean on them. A
    // transmit stage that concludes "no RF" from a missing SWR reading is really
    // reporting "no SWR reading", and the two are only the same thing once the
    // meters have been validated. Where a stage does use a meter, it labels the
    // conclusion meterDependent so a failure can be attributed to the right
    // subsystem instead of the nearest one.
    enum class Phase { Tune, Rx, Tx, Meters, All };

    struct Options {
        Phase phase = Phase::All;
        double frequencyMhz = 14.200;   // mid-band: both sidebands stay in band
        QString mode = QStringLiteral("USB");
        int settleMs = 2500;            // per keyed measurement
        bool includeAudioProbe = true;  // the demodulated-sideband stage

        // The automation power ceiling (AETHER_AUTOMATION_TX_MAX_POWER), or -1
        // for none. This verb keys repeatedly across several stages, so it is
        // the last one that should sit outside the ceiling that exists to stop
        // automation transmitting at the operator's full power.
        int maxPowerPercent = -1;

        // A known off-air carrier for the receive stages. WWV is the default
        // because it is free, always on, on an exactly known frequency, and
        // comes from a source that is not us — which makes it the one external
        // reference available without a second radio.
        double referenceCarrierMhz = 10.000;
        double referenceOffsetHz = 1500.0;   // park the dial this far off it

        // Hard ceiling on RF power for the whole run, as a percentage, or -1 for
        // none. Set from AETHER_AUTOMATION_TX_MAX_POWER by the bridge.
        //
        // The bridge's existing ceiling is applied where a widget setpoint is
        // written, and radiocert keys through its own path — so every keyed stage
        // ran at whatever RF power the operator happened to have set, on the one
        // verb that keys repeatedly and unattended. The ceiling exists precisely
        // so automation cannot do that.
        int maxRfPowerPercent = -1;
    };

    RadioCertification(RadioModel* radio, AudioEngine* audio);

    // Called on every key EDGE (true = keyed, false = unkeyed) so the caller can
    // arm and disarm its own safety machinery per key.
    //
    // The automation server's force-unkey watchdog needs this: it disowns any
    // transmission it finds unkeyed at poll time, so arming once around a
    // diagnostic that unkeys between every stage left the rest of the run
    // unpoliced, and timed the key limit against wall clock rather than
    // continuous key time.
    void setKeyObserver(std::function<void(bool)> observer);

    // Runs the whole sequence synchronously, spinning the event loop between
    // steps. Returns the report. Expect this to take tens of seconds and to key
    // the transmitter repeatedly — the caller is responsible for having decided
    // that is allowed.
    QJsonObject run(const Options& options);

private:
    // One measurement, recorded whether or not it looked healthy.
    //
    // `concern` is the closest thing to a verdict: it is set when a value falls
    // outside what this radio has previously been observed to do, and it names
    // the suspicion rather than declaring failure. `reference` points at the
    // docs/HERMES.md section that explains the failure mode, so the next agent gets
    // the history rather than a bare number.
    //
    // `meterDependent` marks a conclusion that was drawn from meterSnapshot(),
    // and therefore cannot be stronger than the meters themselves — which the
    // Meters phase has not validated yet when the transmit stages run. It is
    // emitted into the report so a reader can attribute a failure to the right
    // subsystem instead of the nearest one.
    void record(const QString& id, const QString& title,
                const QJsonObject& measured, const QString& observation,
                const QString& concern = QString(),
                const QString& reference = QString(),
                bool meterDependent = false);

    // ---- control-plane stages (no DSP, no meters) ----
    void stageModeMap();
    void stageTuning(const Options& o);

    // ---- meter stages, LAST ----
    void stageMeterInventory();
    void stageMeterScale(const Options& o);
    void stageControlEffect(const Options& o);

    // ---- receive stages ----
    //
    // These run FIRST when both phases are selected, and not by accident: the
    // wire's handedness is one fact that transmit and receive both consume, and
    // transmit cannot be reasoned about until it is settled (docs/HERMES.md 15.6).
    void stageConsumerAgreement(const Options& o);
    void stageZeroShift(const Options& o);
    void stageRxSidebands(const Options& o);
    void stagePassbandAfterModeChange(const Options& o);

    // ---- transmit stages, in the order the signal travels ----
    void stagePreconditions();
    void stageControlPlane(const Options& o);
    void stageDspLiveness(const Options& o);
    void stageRf(const Options& o);
    void stageSideband(const Options& o);
    void stageCarrierSuppression(const Options& o);
    void stageLifecycle(const Options& o);

    // Key through TransmitModel, NOT RadioModel::setTransmit.
    //
    // The automation bridge drives RadioModel and the MOX button drives
    // TransmitModel, and three separate bugs reached the operator through that
    // gap (docs/HERMES.md 14.5). A transmit diagnostic that keyed the way only the
    // bridge can would inherit exactly the blindness it exists to remove.
    //
    // Returns whether the radio reached the requested state. Keying can be
    // REFUSED by TransmitModel::runPttPreflight (band limits, interlocks) and
    // requestPttOn returns void, so an unnoticed refusal made every stage below
    // measure an unkeyed radio and blame its own subject for the silence.
    bool keyViaOperatorPath(bool on);
    bool keyedNow() const;

    // Drive used for the sideband probe only. Enough to measure, low enough
    // that the receiver listening to its own transmitter is not driven into
    // clipping — at full power it saturates and the measurement is worthless.
    static constexpr int kSidebandProbePowerPercent = 5;

    // A demodulated tone at or below this is "not recovered". Measured on the
    // HL2: a recovered 1 kHz probe sits near -35 dB and an unrecovered one in
    // the -70s, so the gap is wide and the threshold is not a fine judgement.
    static constexpr double kRecoveredFloorDb = -55.0;

    // Half-width of the search around the expected off-air reference tone. Wide
    // enough for a few ppm of dial error at HF, narrow enough not to collect a
    // neighbouring signal.
    static constexpr double kReferenceSearchSpanHz = 25.0;

    void spin(int ms);
    QJsonObject meterSnapshot() const;

    // WHAT THE OPERATOR'S GAUGE WILL SHOW, read the way the gauge reads it.
    //
    // meterSnapshot() above measures the SEAM — the value that crossed from the
    // backend, by meter index. This measures the CONSUMER: the typed accessors
    // the applets bind to, each behind the liveness gate its real consumer
    // applies. The two answer different questions and a probe that mixes them
    // becomes a third convention that agrees with neither (CERTIFICATION.md
    // 1.34): reading MeterModel::swr() raw reported a confident 1.0:1 in the
    // same stage that reported TX:SWR had never been fed, because an unfed SWR
    // and a perfect match are the same float.
    QJsonObject renderedSnapshot() const;

    // Record forward power seen INSIDE a keyed window. Called from every stage
    // that keys, because "did this radio actually radiate" is a precondition of
    // every transmit-meter verdict and cannot be answered from the meter whose
    // silence is being judged (CERTIFICATION.md 1.37).
    void observeKeyedRf();

    // Was a transmission ever CONFIRMED to have produced RF during this run?
    //
    // Judged on observed forward power, never on requested drive. A drive floor
    // would catch the slider-at-zero case that exposed this and nothing else:
    // an interlock, a band limit or a PA that never enabled are all silent at
    // any slider setting, and all produce the identical "meter never fed".
    bool keyedRfConfirmed() const;

    // The floor forward power must clear for a keyed window to count as RF.
    //
    // Deliberately MeterModel's own SWR-qualifying floor rather than a number
    // chosen here. The finding this precondition exists to suppress is "TX:SWR
    // defined but never fed", and SWR is fed exactly when forward power clears
    // that floor — so any other constant would make the precondition answer a
    // slightly different question from the gate it is reasoning about, which is
    // §1.1 one level up. Zero drive on the HL2 reads 0.001 W; a real key at
    // 50 % read 2.0 W.
    static constexpr double kKeyedRfFloorWatts = MeterModel::kMinForwardWattsForSwr;

    // QPointer, not raw: run() holds these across nested event loops for minutes
    // at a time. If the session tears down mid-run — a disconnect, an app quit —
    // raw pointers would have the remaining stages resume against freed objects.
    // Every stage already opens with a null check, so this costs nothing.
    QPointer<RadioModel> m_radio;
    QPointer<AudioEngine> m_audio;
    std::function<void(bool)> m_onKey;
    int m_keyRefusals = 0;   // keys the radio refused; reported, never ignored
    QJsonArray m_stages;

    // ---- keyed-RF evidence, accumulated across every stage that keys ----
    //
    // Negative means "never sampled": no keyed window found a fresh forward
    // power reading, which is a DIFFERENT state from one that read zero and
    // must not collapse into it.
    double m_keyedFwdWattsMax = -1.0;
    int m_keyedRfSamples = 0;        // fresh forward-power reads inside a key
    int m_keyedWindows = 0;          // keyed windows that reached a measurement
    bool m_fwdPowerMeterDefined = false;

    // The consumer-side reading taken while the radio was actually keyed.
    // stageMeterInventory runs after stageControlEffect has unkeyed and settled
    // 700 ms, so sampling there is sampling the one moment a transmit-only
    // quantity is guaranteed absent (CERTIFICATION.md 1.39).
    QJsonObject m_renderedWhileKeyed;
};

}  // namespace AetherSDR
