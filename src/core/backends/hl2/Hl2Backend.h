#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QTimer>

#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/MetisProtocol.h"   // Hl2Telemetry

#include <deque>
#include <vector>

namespace AetherSDR::hl2 {

class MetisClient;
class Hl2RxDsp;
class Hl2TxDsp;

// IRadioBackend implementation for the Hermes-Lite 2 (HPSDR Protocol 1, raw IQ).
// Owns a MetisClient (UDP wire) and an Hl2RxDsp (demod + panadapter) and maps the
// neutral seam verbs/signals onto them. This is the first backend that owns an
// engine-side DSP chain (RFC §5.5) rather than decoding a cooked stream.
//
// THIS BACKEND CAN KEY THE RADIO. capabilities().canTransmit reports transmit
// AVAILABILITY rather than a constant false: an interactive run may transmit,
// and an automation run defers to the bridge's own TX gate. MetisClient refuses
// independently at the wire, so neither gate is trusted as the only one.
//
// The wire and both DSP chains run on a dedicated I/O thread. That is not only
// about keeping WDSP off the UI: this backend paces EP2, and the gateware
// watchdog halts the stream if EP2 stops arriving.
class Hl2Backend : public IRadioBackend {
    Q_OBJECT

public:
    explicit Hl2Backend(QObject* parent = nullptr);
    ~Hl2Backend() override;

    RadioCapabilities capabilities() const override;
    // Demodulates in-process (Hl2RxDsp); there is no VITA-49 stream at all.
    bool ownsRxAudio() const override { return true; }

    void connectRadio(const RadioConnectRequest& request) override;
    // RFC #4603 PR 3: the client is this radio's memory. Restored state is
    // stashed here pre-connect (validated at this boundary — Principle VII)
    // and applied during connect/pushInitialState; capture reports through
    // currentOperatingState() + operatingStateChanged().
    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setTxSlice(int sliceId) override;
    void setActiveSlice(int sliceId) override;
    void setPanCenter(const QString& panId, double hz) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;

private:
    // The actual span change, after the throttle above has settled. The DDC rate
    // is a RADIO-WIDE register (0x00[25:24]), so this is not per-receiver: every
    // panadapter shares one span.
    void applyPanBandwidth(double hz);

public:
    void setPanFrameRate(const QString& panId, int fps) override;
    bool createPanadapter() override;
    bool removePanadapter(const QString& panId) override;
    void setKeying(bool key) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz) override;
    void setTxPower(int percent) override;
    // No default argument here on purpose: defaults on virtuals bind statically,
    // so repeating the base's is how the two quietly diverge later. The sole
    // call site passes it explicitly.
    void setTune(bool on, int tunePowerPercent) override;
    void setTxFrequency(double hz);
    void setTxDriveLevel(int level);
    // Baseband TX test tone, offsetHz from the carrier, amplitude 0..1.
    // Opt-in only — never enabled by a default.
    void setTxTestTone(double offsetHz, double amplitude);

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;

private:
    // Re-select the companion filter board's band filter for the current slice
    // frequency. Idempotent and change-gated, so it is safe to call from every
    // path that can move the dial.
    void applyBandFilter(const char* reason);
    // Per-band memory (RFC #4603 PR 3): apply the remembered LNA + drive for
    // the band containing freqHz (falling back to the restored defaults),
    // and record the operator's current values into the maps for the band
    // being left. Called from the band-change path and connect.
    void applyPerBandStateFor(double freqHz, const char* reason);
    void applyLnaGainDb(int gainDb);   // the one true LNA application
    void rememberCurrentBandState();
    void notifyOperatingStateChanged();

    void emitSliceState(int ddc);   // sliceChanged(delta) from a receiver's state
    void emitPanState(int ddc);     // panCenterBandwidthChanged from its NCO + rate
    void emitAllSliceState();
    void emitAllPanState();
    void pushInitialState();
    void defineMeters();
    void publishTelemetry(const Hl2Telemetry& t);
    // Clamp 0..100, map onto the drive register, honour the transmit gate.
    // Shared by setTxPower() and setTune() so the mapping exists exactly once.
    void applyDrive(int percent);
    static double temperatureCelsius(int raw);
    // Uncalibrated directional-coupler counts -> watts. See the table in the
    // .cpp for what this curve is and, more importantly, what it is not.
    static double directionalWatts(int raw);
    // Watts -> dBm for the meter seam, floored so 0 W does not become -inf.
    static double wattsToDbm(double watts);

    MetisClient* m_metis = nullptr;
    Hl2TxDsp* m_txDsp = nullptr;
    bool m_connected = false;

    // ---- per-receiver state ----
    //
    // One of these per running DDC. Everything here is genuinely independent
    // between receivers; anything the HARDWARE shares (sample rate, LNA gain,
    // the filter board) stays in the flat members below, and the split between
    // the two is the whole design. Putting a shared register in here would give
    // four receivers four opinions about one piece of hardware, and the last
    // writer would win silently.
    struct Receiver {
        // Owns its own demod + spectrum chain. Created on connect, destroyed on
        // disconnect, and moved to the I/O thread with everything else.
        Hl2RxDsp* dsp = nullptr;

        // Authoritative RX state (HL2 has no status wire echoing it back).
        // The slice's tuned frequency, and — separately — where the DDC's NCO
        // sits. These were one value, which nailed the slice to the centre of
        // the panadapter: every tune moved the NCO, so the pan centre moved with
        // it and the display re-centred under the operator on every click. They
        // are now independent, with the slice tuned inside the passband by a
        // WDSP shift and the NCO moved only when the target would leave the
        // window.
        double sliceFreqHz = 10'000'000.0;   // slice
        double ncoHz       = 10'000'000.0;   // DDC / pan centre

        QString mode = QStringLiteral("USB");
        // Overwritten from defaultPassbandForMode(mode) on the first linkUp of
        // each connect (#4484). Do not treat these initial values as a mode's
        // passband — they match no mode (they equal the unmapped-mode fallback),
        // and when pushInitialState() sent them verbatim a fresh USB connect got
        // DIGU's filter with the mode indicator reading USB.
        int filterLowHz = 150;
        int filterHighHz = 3000;
        // Authoritative AGC state, mirroring the DSP defaults in Hl2RxDsp::Config
        // so the first sliceChanged reports what WDSP was actually opened with.
        QString agcMode = QStringLiteral("med");
        int agcThresholdDb = 65;

        // Host-side per-slice audio. The radio mixes nothing for us — a Flex
        // sums its slices on-radio and sends one stream, and an HL2 demodulates
        // every receiver here — so mute, level and balance are ours to apply.
        //
        // gain is a LINEAR multiplier derived from the operator's 0..100, and
        // pan is 0=left .. 50=centre .. 100=right, matching SliceModel so the
        // seam does not introduce a second scale.
        bool audioMuted = false;
        float audioGain = 1.0f;
        int audioPanPercent = 50;

        // Per-receiver S-meter ballistics. Deliberately NOT shared: a strong
        // signal on receiver 1 must not move receiver 3's needle, which is what
        // a single set of these members would have done.
        QElapsedTimer sMeterClock;
        double sMeterDbm = 0.0;
        bool   haveSMeter = false;
    };
    // GUI THREAD ONLY. Nothing below the seam may touch this — see m_ioDsps for
    // what the sample path reads instead, and publishIoDsps() for why.
    std::vector<Receiver> m_rx;

    // I/O THREAD ONLY: the chains the EP6 fan-out feeds, indexed by DDC.
    //
    // A separate list rather than reaching into m_rx, and the separation is the
    // point. The fan-out used to iterate m_rx directly, which put a GUI-thread
    // container on the sample path: createPanadapter()'s push_back reallocates and
    // removePanadapter()'s erase shifts, either of which can pull the storage out
    // from under a fan-out halfway through it. Rebuilt by publishIoDsps() whenever
    // the receiver set changes — never per packet.
    std::vector<Hl2RxDsp*> m_ioDsps;

    // The four index spaces, never derived from one another. See Hl2Receivers.h.
    // GUI thread only, like m_rx: nothing below the seam reads it, and the
    // per-receiver signal handlers that resolve through it are queued onto this
    // thread.
    Hl2ReceiverMap m_ids;

    // The receiver that owns transmit, and whose slice is the TX slice. The HL2
    // has one transmitter however many receivers it runs, so this is a CHOICE
    // among the receivers rather than a property each of them has.
    int m_txDdc = 0;

    // The receiver the operator is working on. Separate from m_txDdc: you listen
    // on one slice while transmitting on another all the time, and conflating
    // them would drag transmit around every time the operator clicked a pane.
    //
    // Radio-side this means nothing — the HL2 has no notion of a selected
    // receiver. It exists because the CLIENT does: the RX Controls applet, the
    // band buttons, the mode buttons and the meters all act on "the active
    // slice", and on a Flex the radio arbitrates that with `slice set N
    // active=1` and echoes the deselection back. Nothing echoes here, so this
    // is the only thing that can make the answer single-valued.
    int m_activeDdc = 0;

    [[nodiscard]] Receiver* rx(int ddc);
    [[nodiscard]] const Receiver* rx(int ddc) const;
    // Resolve a seam slice id / pan id to a DDC index, or -1. Callers must
    // check: an unknown id means a control for a receiver that is not running,
    // and steering it to receiver 0 would move the wrong panadapter.
    [[nodiscard]] int ddcForSlice(int sliceId) const;
    [[nodiscard]] int ddcForPan(const QString& panId) const;

    // Create/destroy the receiver set. Called on connect once the count is
    // known, and on teardown. Not idempotent by accident: buildReceivers()
    // tears the previous set down first, because a reconnect at a different
    // count must not leave orphaned DSP chains consuming WDSP channel ids.
    void buildReceivers(int count);
    // Create and configure one receiver's DSP chain at `ddc`, wiring its
    // outputs. Shared by buildReceivers() and createPanadapter() so a receiver
    // added at runtime is identical to one built at connect — a second, nearly
    // identical wiring block is exactly how a signal gets connected in one path
    // and forgotten in the other.
    bool openReceiverDsp(int ddc, std::string* error);
    // How many receivers this radio may run right now: the board's reported
    // count, capped by the link budget at the current sample rate.
    [[nodiscard]] int receiverCeiling() const;
    // Re-evaluate the shared band filter and publish the resulting WIDE state.
    void publishWideState();
    // Destroy the DSP chains but KEEP each receiver's operator-set state. The
    // two have different lifetimes — see buildReceivers().
    void releaseReceiverDsps();
    void tearDownReceivers();

    // Hand the I/O thread a fresh copy of the chains to feed. Call after ANY
    // change to the receiver set — one added, one closed, one's DSP replaced.
    //
    // WHY A COPY RATHER THAN SYNCHRONISED ACCESS TO m_rx. Locking m_rx would leave
    // the sharing in place: every present and future reader on either thread would
    // have to know about it, the lock would sit on the per-packet sample path, and
    // taking it in createPanadapter() — which already makes a
    // BlockingQueuedConnection call into the I/O thread — is a deadlock rather
    // than a race. Ordering the two through Qt's event loop instead works, but the
    // happens-before edge lives inside an uninstrumented QtCore, so
    // ThreadSanitizer cannot see it and the weekly sanitizer job could never
    // confirm the fix — it would report the synchronised access as a race forever.
    //
    // Copying removes the sharing outright. m_rx is GUI-thread-only, m_ioDsps is
    // I/O-thread-only, neither thread touches the other's, so there is nothing to
    // order and nothing for a sanitizer to report. The cost is a handful of
    // pointers copied when the operator adds or closes a receiver.
    //
    // BLOCKS until the I/O thread has taken the new list, because callers destroy
    // chains that were in the old one the moment this returns.
    void publishIoDsps();

    // Withdraw EVERY chain from the sample path and block until the I/O thread
    // has taken the empty list. Use this whenever the receiver SET is about to
    // change shape — a close, or a trim after a failed open — because publishing
    // a shortened list while the wire is still sending the old slot count leaves
    // the fan-out mapping slot k to whichever chain moved into index k, which is
    // a live receiver being fed another receiver's IQ.
    //
    // Publishing empty is not just a null-safety measure: it is the only state
    // that is correct no matter what the wire sends next, which is what makes it
    // safe to hold across the receiver-count change. A few milliseconds of
    // silence on the survivors is the cost, and it is the right trade against
    // misfed IQ.
    void withdrawIoDsps();

    // Shared tail of the two above. Takes the list by value so the copy handed
    // to the I/O thread can never alias m_rx.
    void publishIoDspList(std::vector<Hl2RxDsp*> next);

    // Sum one receiver's demodulated audio into the host mix. The HL2 has no
    // on-radio mixer -- a Flex sums its slices and sends one stream -- so with
    // more than one slice open this is where they become one.
    void mixReceiverAudio(int ddc, const std::vector<float>& pcm);

    // Per-slice meter name for the seam ("SLC:LEVEL" for the first receiver, so
    // an existing single-receiver consumer keeps the name it already binds to).
    static QString sliceMeterName(int uiNumber);

    // Mixing scratch. m_mixPending is per receiver and holds demodulated samples
    // waiting for their peers; m_mixAccum is the summing buffer, reused because
    // this runs ~47 times a second per receiver.
    std::vector<std::deque<float>> m_mixPending;
    std::vector<float> m_mixAccum;
    // How far ahead the other receivers may get before a starved one is mixed as
    // silence. Counted in SAMPLES of an interleaved L,R stream at 24 kHz, so
    // 2048 samples is 1024 frames -- ~43 ms, not the ~85 ms a mono reading of
    // the same number would suggest -- long enough to absorb normal
    // WDSP worker jitter, short enough that a genuinely stalled receiver does
    // not hold the speaker silent for a noticeable time.
    static constexpr std::size_t kMixStarvationSamples = 2048;
    // The DDC rate, which IS the panadapter span (emitPanState).
    //
    // Defaults to the NARROWEST the hardware offers, and connectRadio then
    // replaces it with whatever span the operator last chose (Hl2Settings).
    // The widest costs ~8x the narrowest in both directions -- 25.2 vs 3.1 Mbps
    // sustained UDP, 3048 vs 381 packets/second, and 8x the samples through
    // WDSP's decimation front end -- so it is opted into, never imposed at
    // connect on an operator who may be on wifi or a host that cannot carry it.
    //
    // SHARED. 0x00[25:24] is one field for the whole radio, so every receiver
    // runs at the same rate and every panadapter shows the same span. It also
    // bounds the receiver count: see kEp6LinkBudgetFraction and
    // maxReceiversAtRate() — four receivers at 384 kHz is ~89 Mbit/s on the
    // HL2's 100BASE-T and is refused.
    int m_sampleRateHz = 48000;
    // How many receivers to run. Requested by the operator (Hl2Settings),
    // clamped by what the board reports at discovery 0x13 and by the link
    // budget above. Never a hardcoded count — the skimmer gateware variants
    // report 9..12 and the shipping hl2b5up_main reports 4.
    int m_requestedNumRx = 1;
    // What the BOARD said it has (discovery byte 0x13), or 0 when the reply was
    // a short one that omits it. Kept here as well as in MetisClient::Params
    // because createPanadapter() has to answer "may I add one?" on this thread,
    // and the wire object lives on the I/O thread.
    int m_boardMaxRx = 0;
    // What to assume when the board never reported its receiver count — a short
    // discovery reply, or a unicast probe that went unanswered. The shipping
    // hl2b5up_main gateware is built with NR=4 (variants/hl2b5up_main/
    // hermeslite.v), so four is the informed guess rather than the register's
    // encodable maximum. Erring HIGH would stream slots with no DDC behind
    // them — correctly framed, correctly paced, all-zero IQ, which looks
    // exactly like a dead antenna.
    static constexpr int kAssumedBoardMaxRx = 4;
    // Zoom-sweep throttle for setPanBandwidth.
    //
    // Unlike a centre drag, which is cheap to forward, a span change is a
    // BLOCKING WDSP reconfigure plus a settings write. A drag delivers commands
    // every ~33 ms, and a sweep from the narrowest span to the widest crosses
    // every intermediate rate — so the operator paid for two full rebuilds whose
    // results were discarded before either was ever seen. Worse, those rebuilds
    // run on the thread that paces EP2, and the gateware watchdog halts the
    // stream if EP2 stops arriving.
    //
    // Leading edge applies immediately, so a single discrete zoom step still
    // responds at once; anything arriving inside the cooldown is coalesced and
    // the last one applied when it expires. (#4470)
    static constexpr int kBandwidthThrottleMs = 150;
    QTimer* m_bandwidthThrottle = nullptr;
    double m_pendingBandwidthHz = 0.0;   // 0 = nothing coalesced

    // Has this connect already derived the passband from the mode? (#4484)
    //
    // pushInitialState() runs on every linkUp, and MetisClient re-emits linkUp
    // after an EP6 silence timeout with no new connectRadio(). Without this the
    // derivation would reset an operator's own filter edit on a transient glitch.
    // Cleared in connectRadio(), so a genuine reconnect re-derives.
    //
    // Radio-wide rather than per receiver, unlike the mode and passband it
    // guards (those moved into Receiver): it gates the once-per-connect
    // derivation PASS, which now runs over every receiver.
    bool m_passbandDerivedThisConnect = false;

    // ---- SHARED HARDWARE ----
    //
    // The HL2 has ONE AD9866. Every receiver is a DDC behind that single
    // converter, so these are radio-wide and cannot be made per-receiver however
    // much the UI would like them to be. Four panadapters on four bands share
    // one preamp setting and one filter selection; see applyBandFilter() for
    // what happens when they disagree.
    int m_lnaGainDb = 20;
    // Last J16 open-collector filter byte commanded. 0xFF is "nothing sent yet"
    // rather than a real selection — kOcNone (0x00) is a legitimate value
    // meaning "every relay released", so it cannot double as the sentinel.
    int m_ocFilterByte = 0xFF;
    // Owns the LNA gain <-> dBm coupling so a gain change cannot move the trace.
    Hl2DbReference m_dbRef;

    // The wire and the DSP both live here, off the GUI thread. See MetisClient's
    // header for why the EP2 pacer in particular must not share a thread with
    // the UI. Owned by this object; joined in the destructor.
    QThread* m_ioThread = nullptr;

    // Process-wide transmit availability, decided once at construction:
    // interactive runs may transmit; automation runs defer to the bridge's
    // AETHER_AUTOMATION_ALLOW_TX gate. Mirrored into MetisClient, which refuses
    // independently at the wire.
    bool m_txAllowed = false;
    Hl2Telemetry m_telemetry;
    // Cumulative EP6 sequence gaps, mirrored onto THIS thread from
    // MetisClient::dropsUpdated. Deliberately a copy rather than a call into
    // MetisClient::droppedPackets(): that object lives on the I/O thread, and
    // healthSnapshot() is read from the GUI thread.
    quint64 m_drops = 0;
    bool m_adcOverload = false;
    bool m_keyed = false;
    bool m_tuning = false;
    bool m_toneFromTune = false;
    // Last drive the operator asked for through setTxPower(), so TUNE can drop to
    // tune power and put it back on release. Seeded to the same value
    // TransmitModel defaults rfPower to, so a TUNE before any power change
    // restores something sane rather than 0.
    int m_rfPowerPercent = 100;
    // RFC #4603 PR 3 state memory. m_restoredState is the validated snapshot
    // handed over pre-connect; the per-band maps are the working copies the
    // session reads and updates (band key -> value; see Hl2Bands.h). Defaults
    // apply to bands never visited. m_currentBandKey tracks which band's
    // entries the operator's live edits belong to.
    bool m_haveRestoredState = false;
    RestoredRadioState m_restoredState;
    QMap<QString, int> m_lnaDbByBand;
    QMap<QString, int> m_driveByBand;
    int m_lnaDefaultDb = 20;         // matches m_lnaGainDb's own default
    int m_driveDefaultPercent = -1;  // <0: no restored default; leave drive alone
    QString m_currentBandKey;
    // True while band-memory / restore code drives setTxPower() itself: the
    // internal application must neither bootstrap the operator baseline nor
    // record into the per-band map — only OPERATOR intent does that.
    bool m_applyingBandMemory = false;
    // Tune-carrier amplitude, full scale into the modulator. Actual radiated
    // power is governed by the TX drive register, which is where an operator
    // sets it; scaling here as well would make the power control non-linear for
    // no reason.
    static constexpr double kTuneCarrierAmplitude = 1.0;
    int m_lastFwdRaw = -1;

    // ---- Meter pacing / ballistics ----
    //
    // WDSP hands us a signal-strength reading once per demodulated block, which
    // at 24 kHz output is ~47 a second and scales with the span. Every one of
    // them crossed the thread boundary into MeterModel and repainted the
    // S-meter, so the needle was being driven far faster than it can be read
    // and far faster than a Flex drives the same widget.
    //
    // Two separate things fix that and they are NOT interchangeable:
    //   - the RATE gate below decides how often a value is published;
    //   - the EMA decides what value gets published when it is.
    // Dropping samples without smoothing would alias — the meter would show
    // whichever instant happened to land on the tick.
    //
    // 100 ms is the cadence MetisClient already publishes radio telemetry at
    // (kTelemetryMinIntervalMs), so every HL2 meter now updates on one clock.
    static constexpr qint64 kMeterPublishIntervalMs = 100;
    // Flex's own meter ballistics, from MeterModel's forward-power smoothing:
    // fast attack so a peak is not missed, slow decay so the needle settles.
    // Reused rather than re-invented so an operator moving between a Flex and
    // an HL2 sees meters that behave the same way.
    static constexpr double kMeterAttackAlpha = 0.5;
    static constexpr double kMeterDecayAlpha  = 0.15;
    // The S-meter's clock and EMA are PER RECEIVER (Receiver::sMeter*). Sharing
    // them would let a strong signal on one receiver drive every other
    // receiver's needle, and the 100 ms rate gate would publish whichever
    // receiver's block happened to land on the tick.
    //
    // PA temperature rides the 10 Hz telemetry, so it needs no rate gate of its
    // own — but the instrumentation ADC's low bits are noisy enough that the
    // displayed value flickered by a degree at rest. Same EMA, symmetric:
    // heating and cooling are both slow and neither deserves a fast attack.
    static constexpr double kPaTempAlpha = 0.2;
    double m_paTempC = 0.0;
    bool   m_havePaTemp = false;

    // Fraction of the half-span the slice may occupy before the NCO re-centres.
    // 0.8 leaves the outer 20% of each side for filter roll-off.
    // Slice AGC threshold (0..100) -> WDSP gain ceiling in dB. 0.6 spans
    // 0..60 dB; see the measurement in setSliceAgc().
    static constexpr double kAgcCeilingDbPerUnit = 0.6;
    static constexpr double kUsablePassbandFraction = 0.8;
    // Ceiling on host-mixed slice audio. N demodulated receivers are summed
    // here, so N loud slices can sum past full scale where one never could.
    static constexpr float kMixCeiling = 1.0f;
    // Centre of SliceModel's 0..100 balance range.
    static constexpr int kAudioPanCentre = 50;

    // AD9866 LNA gain limits, in dB. These are the range ccRxGain() encodes
    // (C4 = 0x40 | (dB + 12), a 6-bit field), so they are the register's own
    // limits rather than a policy choice — clamping anywhere else would let a
    // value be silently truncated on the wire instead of stopping at the end of
    // the slider's travel.
    static constexpr int kLnaGainMinDb  = -12;
    static constexpr int kLnaGainMaxDb  = 48;
    static constexpr int kLnaGainStepDb = 1;
};

}  // namespace AetherSDR::hl2
