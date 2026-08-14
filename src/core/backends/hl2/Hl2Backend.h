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
#include <limits>
#include <memory>
#include <utility>
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
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
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
    void createNotch(double centerHz, double widthHz) override;
    void setNotch(int notchId, const AetherSDR::NotchDelta& delta) override;
    void removeNotch(int notchId) override;
    void setNotchesEnabled(bool on) override;
    void setKeying(bool key) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz) override;
    void setTxPower(int percent) override;
    void setTxFilter(int lowHz, int highHz) override;
    void setMicGain(int level) override;
    // No default argument here on purpose: defaults on virtuals bind statically,
    // so repeating the base's is how the two quietly diverge later. The sole
    // call site passes it explicitly.
    void setTune(bool on, int tunePowerPercent) override;
    void setTxAudioMonitor(bool on) override;
    void setTxFrequency(double hz);
    void setTxDriveLevel(int level);
    // Baseband TX test tone, offsetHz from the carrier, amplitude 0..1.
    // Opt-in only — never enabled by a default.
    void setTxTestTone(double offsetHz, double amplitude);

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;
    LinkStats linkStats() const override;

signals:
    // Connect-time progress for the CLIENT-SIDE DSP build, and deliberately not
    // on the IRadioBackend seam: WDSP is this backend's alone (a Flex
    // demodulates in firmware, an Icom does not use WDSP at all), so a neutral
    // signal would be one every other family had to ignore.
    //
    // `stage` is already operator-facing text, and carries NO counter of its own
    // — `done`/`total` are the counter, so the label owns how (or whether) a
    // fraction is rendered. They count WDSP channel opens, which means receivers
    // and only receivers: the transmit chain designs FIR kernels and opens
    // nothing, so it is not a step and is not in `total`.
    //
    // Emitted from the GUI thread, including the terminal one, so a slot may
    // touch widgets directly.
    void dspSetupProgress(const QString& stage, int done, int total);
    void dspSetupFinished();

private:
    // Publish linkStats() on the fixed cadence the seam promises. Driven by a
    // timer here rather than by MetisClient's receive path so the tick survives
    // the radio going silent — which is the case the heartbeat has to detect.
    void publishLinkStats();
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

    // ---- the connect's three phases ----
    //
    // connectRadio() used to be one straight-line function that waited on the
    // I/O thread through Qt::BlockingQueuedConnection for every WDSP open. The
    // work was correctly OFF the GUI thread; the GUI thread simply stood there
    // holding its breath for all of it. On a machine with no FFTW wisdom yet
    // that is tens of seconds of an application that answers nothing at all —
    // measured at 21-82 s here, load-dependent, with the automation bridge (a
    // GUI-thread server) going completely silent for the whole window.
    //
    // So it is split. connectRadio() itself still does every member seeding
    // SYNCHRONOUSLY — hl2_state_restore_test reads currentOperatingState()
    // immediately after it returns, and that contract is worth keeping — and
    // then hands the channel opens to beginDspSetup() and returns to the event
    // loop. finishDspSetup() resumes on the GUI thread when the opens are done
    // and starts the wire.
    //
    // What did NOT change is the ORDER: every DSP chain is still open and
    // configured before MetisClient::start(), because EP2 must not stop (see
    // the note above buildReceivers() and HERMES.md §20.8). The sequence stays
    // serial on the I/O thread; only the GUI thread stopped waiting for it.
    void beginDspSetup();

    // Everything the async build needs to carry across event-loop turns (the
    // wire params, the RX and TX configs, and which connect it belongs to). A
    // local could not: connectRadio() has returned long before the I/O thread
    // answers. Defined in the .cpp so this header keeps its forward
    // declarations of MetisClient/Hl2RxDsp/Hl2TxDsp instead of including all
    // three for three member types.
    struct PendingConnect;
    struct DspSetupResult;
    void finishDspSetup(const DspSetupResult& result);

    std::unique_ptr<PendingConnect> m_pendingConnect;
    // A connect that arrived while m_pendingConnect was still building. Held
    // rather than served inline — see the guard at the top of connectRadio().
    std::unique_ptr<RadioConnectRequest> m_queuedConnect;
    quint64 m_connectGeneration = 0;

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

    // ---- manual frequency calibration (Hl2FreqCal) ----
    //
    // EVERY frequency that leaves for the radio goes through these two. That is
    // the point of them: the correction is a single scalar (see Hl2FreqCal for
    // why), so it needs exactly one entry point per direction, and a write site
    // that bypasses them leaves a receiver tuned somewhere other than the rest
    // of the radio believes — with nothing to indicate it, because no NCO
    // register can be read back.
    //
    // Receiver::sliceFreqHz and ncoHz stay in the TRUE-RF domain. Only the
    // values handed to the wire and to the DSP are scaled, so the panadapter
    // axis, the band-filter decisions and every emitted state keep reading in
    // real frequency.

    // NCO register value (0x01 TX, 0x02+ RX) for a true-RF frequency.
    [[nodiscard]] std::uint32_t ncoCommandHz(double trueHz) const noexcept;
    // DSP shift that puts sliceTrueHz at baseband, given the true-RF NCO. Rounds
    // the NCO command first and computes the shift against that rounded value,
    // so the register's 1 Hz quantisation cancels instead of leaking into audio.
    [[nodiscard]] double dspShiftHz(double sliceTrueHz, double ncoTrueHz) const noexcept;
    // Re-send every NCO (RX banks + TX) from the receivers' unchanged true-RF
    // state. Called when the calibration changes mid-session, so the operator
    // hears the correction move while they are nulling a beat note rather than
    // on their next tune.
    void repushAllFrequencies();
    // Clamp, persist, adopt, re-push — the single path for a calibration change
    // whoever asked for it (setup dialog, automation bridge, connect).
    void applyFreqCalPpb(int ppb, bool persist);

    // The operator's calibration for THIS radio and the derived scale applied to
    // every commanded frequency. 0 / 1.0 is "uncalibrated" — the behaviour every
    // build before this one had.
    int m_freqCalPpb = 0;
    double m_freqCalScale = 1.0;
    // Identity of the connected radio (its MAC, from the connect request), so
    // the calibration loads and stores per radio rather than globally: it
    // describes one physical crystal. Empty until connectRadio().
    QString m_radioSerial;

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

    // ── Manual notches ────────────────────────────────────────────────────
    //
    // The authoritative notch set, and the thing that reconciles two different
    // ways of naming a notch. Above the seam a notch has a STABLE id that never
    // changes; inside WDSP it has a POSITIONAL index that shifts every time an
    // earlier notch is deleted. Keeping the vector in the same order WDSP keeps
    // its database means the index is simply the position here, so the mapping
    // is a lookup rather than a second table that can fall out of step.
    //
    // Notches are RADIO-WIDE, not per-receiver: an interferer is a fact about
    // the band, so every receiver gets the same set applied to it. That also
    // means a receiver created later has to be seeded (seedNotches).
    //
    // GUI thread only, like m_rx.
    struct NotchRecord {
        int id = 0;
        double centerHz = 0.0;
        double widthHz = 0.0;
        bool active = true;
    };
    std::vector<NotchRecord> m_notches;
    // Never reused, even after a removal. A recycled id would let a stale
    // reference from the UI address a different notch than it meant to.
    int m_nextNotchId = 1;
    bool m_notchesEnabled = true;

    // Index of `notchId` in m_notches — which IS its WDSP handle — or -1.
    [[nodiscard]] int notchIndexFor(int notchId) const;
    // Push the whole notch set + tune frequency into one receiver's chain. Used
    // when a receiver appears after the notches did.
    void seedNotches(const Receiver& r);
    // Re-point one receiver's notch axis at its current NCO. Called wherever
    // ncoHz changes; without it the notches stay where the NCO used to be.
    void pushNotchTune(const Receiver& r);

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
    // Transport counters, mirrored onto THIS thread from
    // MetisClient::linkCountersUpdated for the same reason m_drops is.
    //
    // Held as the SEAM's type rather than the client's: MetisClient is only
    // forward-declared here, so a nested type of it cannot be a member, and
    // translating at the receive lambda (where MetisClient.h is included) keeps
    // the wire-shape-to-seam-shape mapping in exactly one place.
    LinkStats m_link;
    QTimer* m_linkStatsTimer = nullptr;
    // rxPackets as of the PREVIOUS tick. The difference is the only thing that
    // can answer "is the radio still sending", which a cumulative total cannot.
    quint64 m_linkRxPacketsAtLastTick = 0;
    static constexpr int kLinkStatsIntervalMs = 1000;
    bool m_adcOverload = false;
    bool m_keyed = false;
    bool m_tuning = false;
    bool m_txMonitor = false;
    bool m_toneFromTune = false;
    // Last drive the operator asked for through setTxPower(), so TUNE can drop to
    // tune power and put it back on release. Seeded to the same value
    // TransmitModel defaults rfPower to, so a TUNE before any power change
    // restores something sane rather than 0.
    int m_rfPowerPercent = 100;
    // The APPLIED side of the pair above, for the health snapshot (#4912).
    // The raw 0..kTxDriveMax value setTxDriveLevel() last handed to MetisClient —
    // negative means never written, so the row stays absent rather than claiming a
    // 0 the radio was never told. (Its companion "gated" row is derived from
    // m_txAllowed at read time, not latched here — see healthSnapshot().)
    int m_txDriveRegister = -1;
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

    // The operator's TX passband, once they have set one, and the flag that says
    // they have.
    //
    // The flag is the load-bearing half. defaultTxPassbandForMode() is re-pushed
    // on every mode set and every transmit-slice move — deliberately, so a fresh
    // session is sideband- and mode-correct from the first key — and it has no
    // way to tell "nobody has chosen" from "the operator chose 300..2700". Without
    // this, an eSSB passband survives until the next mode change and is then
    // silently replaced by the voice default, which looks like the control
    // working and then randomly forgetting.
    bool m_txFilterFromOperator = false;
    int m_txFilterLowHz = 300;
    int m_txFilterHighHz = 2700;

    // Loudest microphone peak of the current transmission, in dBFS, so setKeying()
    // can tell at unkey whether the operator spent the whole of it below the ALC's
    // hold threshold — the one case where holding the gain leaves them quiet
    // rather than merely stopping the stage pumping. -140 is the floor
    // Hl2TxDsp::micPeak reports for silence, and means "nothing measured yet".
    float m_txMicPeakMaxDbfs = -140.0f;

    // The passband to push at the modulator for `mode`: the operator's if they
    // have chosen one, otherwise that mode's default.
    std::pair<int, int> effectiveTxPassband(const QString& mode) const;
    // Apply that passband AND announce it as a TransmitDelta, so the Phone
    // applet's cut readout matches what the transmitter is running rather than
    // what was last asked for. See the definition for why setTxFilter() is the
    // one push that does not go through here.
    void pushTxPassband(const QString& mode);
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

    // ---- Forward-power peak hold ----
    //
    // WHY THE SHARED BALLISTICS ABOVE ARE NOT ENOUGH HERE, and why this is a
    // peak ESTIMATE rather than a peak measurement.
    //
    // A Flex reports FWDPWR from a detector the radio itself peak-reads, so
    // MeterModel's fast-attack EMA is smoothing an already-peak-tracking
    // signal. The HL2 has nothing of the kind: forward power is one 12-bit
    // conversion from the `slow_adc` I2C converter, round-robined with reverse
    // power, temperature and bias current, with no peak detector and no
    // averaging anywhere in the gateware (rtl/slow_adc.v, rtl/control.v ~L262 —
    // tier 1 on the source-precedence ladder). Each reading is the RF envelope
    // at whatever instant the I2C transaction happened to land, and we see one
    // every kTelemetryMinIntervalMs.
    //
    // Speech peaks last tens of milliseconds. Sampling that envelope at 10 Hz
    // lands on a peak essentially never, so an SSB reading sat 8-12 dB below
    // PEP while a constant-envelope FT8 or WSPR transmission — where every
    // instant IS the peak — read full scale. That is the whole of the reported
    // "6 W on FT8, 1 W on voice": both were making the same PEP.
    //
    // No host-side filter can recover a peak that was never sampled. What a
    // hold CAN do is accumulate the maximum across a transmission: ~30
    // independent samples in a 3 s over lands within a few dB of true PEP, and
    // converges further the longer the operator talks. So this is honest as an
    // estimate that settles, and dishonest as an instantaneous reading — which
    // is why the meter description says so and why the raw counts keep being
    // logged and published for the bridge alongside it.
    //
    // Instant attack, slow release, in WATTS rather than counts because the
    // calibration curve is markedly non-linear and a peak held in counts would
    // decay at a rate that changed with level.
    //
    // 0.05 per 10 Hz sample is a ~2 s release, matching what an outboard PEP
    // wattmeter does. Slower would keep a peak past the end of the over; faster
    // would decay between syllables and give the reading back to the average,
    // which is exactly the failure this exists to fix.
    //
    // ONE DOWNSTREAM CONSEQUENCE, stated because it is not obvious: TxApplet
    // runs its own ~2 s PEP hold + linear decay on the FWDPWR gauge (#2561),
    // fed from MeterModel::txPeakChanged and documented there as taking the
    // "pre-smoothed" sample. On this backend that sample is now itself held, so
    // the applet's PEP tick and the gauge fill converge on the same number
    // where they diverge on a Flex. That is the honest outcome rather than a
    // defect — the fill is a peak estimate here BECAUSE the hardware gives us
    // nothing to smooth — but the applet's tick carries no extra information on
    // an HL2, and anyone reading the two as independent would be wrong.
    static constexpr double kFwdPeakReleaseAlpha = 0.05;
    double m_fwdPeakWatts = 0.0;

    // Operator's MIC slider position, 0..100. Kept alongside the value pushed
    // into the modulator so the automation bridge and any diagnostic can report
    // what the operator asked for, not just the linear gain it became — the two
    // are related by a mapping that is easy to get backwards when reading a log.
    // 50 is unity; see setMicGain().
    int m_micLevel = 50;

    // ---- Voice-chain mirrors, for healthSnapshot() ----
    //
    // Same reason as m_drops and m_linkCounters above: these originate on the
    // DSP worker, and healthSnapshot() is called from the GUI thread. Mirroring
    // on signal delivery means the readout is a plain read of a value that
    // already lives on the reading thread, instead of reaching across for it.
    //
    // Both are published as meters too. They are ALSO kept here because a meter
    // is a stream nobody can query after the fact, and the whole point of
    // exposing these is answering "what was the chain doing on that over?" — a
    // question the operator asks once the over is finished.
    //
    // NaN, not 0, for "never reported": the ALC applying 0 dB is a real and
    // common state, so a zero default would be indistinguishable from a
    // modulator that has never run.
    double m_alcGainDb = std::numeric_limits<double>::quiet_NaN();
    double m_alcPeakDbfs = std::numeric_limits<double>::quiet_NaN();
    // The linear gain the MODULATOR holds, echoed back by Hl2TxDsp rather than
    // computed here. NaN until the modulator has confirmed one, so "the push
    // never landed" is distinguishable from "it landed at unity" — which is the
    // exact pair that was indistinguishable while this control was dead.
    double m_appliedMicGainLinear = std::numeric_limits<double>::quiet_NaN();

    // The ALC hold threshold the modulator was CONFIGURED with, captured from
    // the Config that connectRadio() hands it. Read by healthSnapshot() and by
    // setKeying()'s "raise mic gain" diagnostic, both of which previously
    // re-derived it from a default-constructed Config and so would have gone on
    // reporting -45 dBFS the day connectRadio() set the field to anything else.
    //
    // Seeded with Config's own default so a snapshot taken before the first
    // connect still reports what the modulator would use. The literal is spelt
    // out because Hl2TxDsp is only forward-declared in this header — a
    // static_assert in Hl2Backend.cpp pins it to Config's default, so the two
    // cannot drift silently.
    double m_alcHoldBelowDbfs = -45.0;

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

    // The TX passband's ceiling: Nyquist of the TX AUDIO rate, which is
    // AudioEngine's 24 kHz — NOT of the 48 kHz EP2 rate. The modulator
    // interpolates, so what bounds the passband is what the input can carry.
    //
    // One constant because setTxFilter() and applyRestoredState() must agree:
    // a restore bound looser than the setter's would admit a persisted value the
    // operator could not have produced, and a tighter one would silently discard
    // a setting they did.
    static constexpr int kTxAudioMaxHz = 12000;
};

}  // namespace AetherSDR::hl2
