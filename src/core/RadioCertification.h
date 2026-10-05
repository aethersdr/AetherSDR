#pragma once

#include "models/MeterModel.h"   // kMinForwardWattsForSwr — the keyed-RF floor
#include "TxCoordinator.h"
#include "models/TxController.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace AetherSDR {

class RadioModel;
class AudioEngine;

// Transmit bring-up instrument: drives the whole TX chain against one radio
// (no simulator, no second receiver) and reports what every stage did.
// A diagnostic, not a certification: no pass/fail, because thresholds for one
// radio are guesses for the next. Stages follow docs/HERMES.md section 14 and
// each result carries its reference. The sideband stage demodulates our own
// TX rather than reading the panadapter, which shares the transmitter's
// convention and so can't detect an inversion (14.6); it is still weaker than
// an unrelated receiver, hence the manual checks at the end of the report.
class RadioCertification {
public:
    // Bring-up order; each phase depends only on earlier ones:
    //   Tune    control plane only — no DSP, no audio, no meters
    //   Rx      demodulation and handedness — audio evidence, still no meters
    //   Tx      keying and modulation — audio evidence where it exists
    //   Meters  the instruments themselves, LAST, against known stimuli
    // Earlier phases must not lean on meters; where they do, the result is
    // marked meterDependent.
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

        // A steady carrier for stage-squelch-scale, or 0 for referenceCarrierMhz.
        // Choose one below S9: above it the IC-7300MK2's gate sits 6-10 dB off
        // its own measured line, and Flex's scale happens to land near both.
        double squelchCarrierMhz = 0.0;

        // Whether the operator's Auto SQL is engaged on the slice under test;
        // nullopt when it cannot be observed. Auto writes the slice on every pan
        // frame, so the squelch stages decline while it is on. Supplied by the
        // caller because the intent lives in the GUI, not in any model.
        std::function<std::optional<bool>()> autoSquelchEngaged;
    };

    RadioCertification(RadioModel* radio, AudioEngine* audio,
                       std::shared_ptr<TxController> controller = {});

    // Called per key request (true = on, false = off or refused) so the caller
    // can police the admitted operation. This is not qualified RF readback.
    // A diagnostic may spend a long time idle between keys: arming once around
    // the whole run would time the diagnostic, not an individual transmission.
    // Key-on notification is after synchronous admission, before any event-loop
    // wait. Previous identity/state let the observer reject an unrelated over.
    using KeyObserver = std::function<void(bool, const TxCoordinator::Operation&, bool)>;
    void setKeyObserver(KeyObserver observer);

    // Runs the whole sequence synchronously, spinning the event loop between
    // steps. Returns the report. Expect this to take tens of seconds and to key
    // the transmitter repeatedly — the caller is responsible for having decided
    // that is allowed.
    QJsonObject run(const Options& options);

private:
    friend class RadioCertificationTestAccess;
    // One measurement, recorded whether healthy or not. `concern` names a
    // suspicion when a value falls outside previously observed behaviour (never a
    // verdict); `reference` points at the docs/HERMES.md section. `meterDependent`
    // marks a conclusion drawn from meterSnapshot(), which the Meters phase has
    // not yet validated when TX stages run.
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

    // ---- what the operator reads off a control: non-keying, before the above ----
    //
    // A scaled control's readout, an interlocked button and the SQL line are
    // instruments too, and each was wrong on the IC-7300MK2 while the backend's
    // own tests agreed with it (CERTIFICATION.md 1.41-1.43).
    void stageControlDomain(const Options& o);
    void stageFrontEndInterlock();
    void stageSquelchScale(const Options& o);

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

    // Key through TransmitModel (the MOX button's path), not
    // RadioModel::setTransmit (the bridge's), which differ (docs/HERMES.md 14.5).
    // Returns whether the radio reached the requested state: runPttPreflight can
    // refuse keying (band limits, interlocks) and requestPttOn returns void.
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

    // What the operator's gauge shows: the typed MeterModel accessors applets
    // bind to, each behind its consumer's liveness gate. meterSnapshot() instead
    // measures the seam by meter index. Don't mix them: raw MeterModel::swr()
    // reads 1.0 when unfed (CERTIFICATION.md 1.34).
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
    const std::shared_ptr<TxController> m_txController;
    TxController::Input m_keyInput;
    KeyObserver m_onKey;
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
