#pragma once

#include "core/backends/AutoRfGainControl.h"
#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QTimer>

#include "core/backends/hl2/Hl2AdcPairing.h"
#include "core/backends/hl2/Hl2AutoGainPolicy.h"
#include "core/backends/hl2/Hl2BandMemoryPolicy.h"
#include "core/backends/hl2/Hl2CapabilityAnnouncer.h"
#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/Hl2HardwareOptions.h"
#include "core/backends/hl2/Hl2IoBoardPolicy.h"
#include "core/backends/hl2/Hl2TelemetryCadence.h"  // Hl2LinkState (#15)
#include "core/backends/hl2/Hl2TelemetryService.h"  // borrowed, owned by RadioModel
#include "core/backends/hl2/Hl2TelemetrySource.h"   // the shared attribution rule
#include "core/backends/hl2/Hl2RateCommit.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/MetisProtocol.h"   // Hl2Telemetry

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <utility>
#include <optional>
#include <vector>

namespace AetherSDR::hl2 {

class MetisClient;
class Hl2RxDsp;
class Hl2TxDsp;

// The IQ rates the HL2's DDC can run, ascending. One list serves as the
// capability advertisement (sampleRatesHz), the pan zoom limits and the snap set
// (nearestIqSampleRateHz), because on this radio the pan span IS the sample rate
// (Hl2Backend::emitPanState). In the header so the capability test asserts
// against this array, not a copy.
inline constexpr int kIqSampleRatesHz[] = {48000, 96000, 192000, 384000};

// The wideband converter view this backend declares when connected. A free
// function so a typo in the named extension verb is caught by a socket-free test.
[[nodiscard]] AetherSDR::WidebandConverterView widebandConverterViewRecord() noexcept;

// IRadioBackend for the Hermes-Lite 2 (HPSDR Protocol 1, raw IQ). Owns a
// MetisClient (UDP wire) and the Hl2RxDsp/Hl2TxDsp chains (RFC §5.5).
// THIS BACKEND CAN KEY THE RADIO. capabilities().canTransmit reports TX
// availability; automation defers to the bridge's TX gate and MetisClient
// refuses independently at the wire, so neither gate is the only one.
// Wire and DSP run on a dedicated I/O thread; this backend paces EP2, and the
// gateware watchdog halts the stream if EP2 stops. IAutoRfGainControl methods
// are thin forwarders to this class's own names.
class Hl2Backend : public IRadioBackend, public IAutoRfGainControl {
    Q_OBJECT

public:
    explicit Hl2Backend(QObject* parent = nullptr);
    ~Hl2Backend() override;

    RadioCapabilities capabilities() const override;
    // Demodulates in-process (Hl2RxDsp); there is no VITA-49 stream at all.
    bool ownsRxAudio() const override { return true; }

    void connectRadio(const RadioConnectRequest& request) override;
    // RFC #4603: the client is this radio's memory. Restored state is validated and
    // stashed here pre-connect, applied during connect/pushInitialState; capture
    // reports through currentOperatingState() + operatingStateChanged().
    void applyRestoredState(const RestoredRadioState& state) override;
    // The validated document applyRestoredState() kept (test seam).
    // currentOperatingState() reads the receivers, seeded only at linkUp, so
    // pre-connect tests assert here instead (#5031).
    const RestoredRadioState& restoredStateForTest() const { return m_restoredState; }
    RestoredRadioState currentOperatingState() const override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    // `requested`: alias spellings (CWU/NFM/WFM) are canonicalised onto
    // publishedModeStrings() before a slice holds them.
    void setSliceMode(int sliceId, const QString& requested) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setCwPitch(int hz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    // Impulse noise blanker, run in host WDSP (the HL2 has no firmware DSP). NR and
    // ANF are deliberately not implemented and stay hidden.
    void setSliceNoiseBlanker(int sliceId, bool on, int level) override;
    void setSliceSquelch(int sliceId, bool on, int level) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setTxSlice(int sliceId) override;
    void setActiveSlice(int sliceId) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setAutoRfGain(bool on);

private:
    // The span change after the throttle settles. The DDC rate is radio-wide
    // (0x00[25:24]), so every panadapter shares one span.
    void applyPanBandwidth(double hz);

public:
    void setPanFrameRate(const QString& panId, int fps) override;
    // The operator's FFT AVG (0..100) and weighted toggle. This backend owns
    // the panadapter's averaging (RFC #5782), so both land in the receiver's
    // Hl2Spectrum; see averageTimeMsForStep() and Hl2Spectrum::setAverageTimeMs().
    void setPanAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;

    // One FFT AVG step is 10 ms of averaging time constant, so 0..100 spans
    // 0..1 s: the ANAN's unit (AnanBackend's kMsPerAverageStep), so one setting
    // means one time constant on both families. A time, not a frame count: a
    // depth in frames would move with the fps slider. It does not match a Flex,
    // whose `average=` has no documented unit.
    static constexpr int kMsPerAverageStep = 10;
    [[nodiscard]] static constexpr int averageTimeMsForStep(int average) noexcept
    {
        return (average < 0 ? 0 : (average > 100 ? 100 : average)) * kMsPerAverageStep;
    }
    bool createPanadapter() override;
    bool removePanadapter(const QString& panId) override;
    void createNotch(double centerHz, double widthHz) override;
    void setNotch(int notchId, const AetherSDR::NotchDelta& delta) override;
    void removeNotch(int notchId) override;
    void setNotchesEnabled(bool on) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setCwKeying(bool down, bool breakIn, int breakInDelayMs, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                       TxAudioSource source,
                       const TxCoordinator::Context& context) override;
    void setTxPower(int percent) override;
    void setTunePower(int percent) override;
    void setTxFilter(int lowHz, int highHz) override;
    void setMicGain(int level) override;
    // No default argument: defaults on virtuals bind statically and would diverge
    // from the base's. The sole call site passes it explicitly.
    void setTune(bool on, int tunePowerPercent, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setTxAudioMonitor(bool on) override;
    void setTxFrequency(double hz);
    void setTxDriveLevel(int level);
    // Baseband TX test tone, offsetHz from the carrier, amplitude 0..1.
    // Opt-in only — never enabled by a default.
    void setTxTestTone(double offsetHz, double amplitude, const TxCoordinator::Operation& operation);

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;
    QVariantList dspChains() const override;

    // LNA gain split (Hl2GainSplit.h). `lnaBaselineDb` is the operator's: stored per
    // band, persisted, written only by setPanRfGain and band-memory restore.
    // `lnaAutoOffsetDb` is a non-negative session-only attenuation below it, owned by
    // the automatic control. The register, Hl2DbReference and panRfGainChanged carry
    // `lnaEffectiveDb`. An offset of 0 restores the operator's number in one action.
    void setLnaAutoOffsetDb(int offsetDb);
    [[nodiscard]] int lnaAutoOffsetDb() const noexcept { return m_lnaAutoOffsetDb; }
    [[nodiscard]] int lnaBaselineDb() const noexcept { return m_lnaGainDb; }
    [[nodiscard]] int lnaEffectiveDb() const noexcept;
    [[nodiscard]] bool autoRfGainEnabled() const noexcept { return m_autoRfGainEnabled; }

    // Max attenuation the loop may apply, in dB below the operator's baseline. The
    // floor belongs to the law: 24 dB for "bandscope" (what a backend starts with)
    // and "probe", 26 for "ramp" and "binary". The 26 was sized from #5354's sweep,
    // whose gain labels read 32 dB low (one unit's defect, #5943), so it is not a
    // measured bound. Everything else in Hl2AutoGainPolicy.h is deliberately not
    // operator-settable.
    void setAutoRfGainFloorDb(int floorDb);
    [[nodiscard]] int autoRfGainFloorDb() const noexcept
    {
        return m_autoGainConfig.maxOffsetDb;
    }
    // The deepest floor the operator may configure: the whole native span, so
    // from any armable baseline the loop can be allowed to dig to the register
    // floor. The default floor belongs to the installed law (above).
    static constexpr int kAutoRfGainFloorMaxDb = hl2::kLnaGainMaxDb - hl2::kLnaGainMinDb;

    // Which Hl2AutoGainPolicy.h configuration the loop runs:
    // "bandscope" (default) bandscopeReleaseConfig(): probing law whose release
    //             needs a measured wideband headroom reading; arms the bandscope gate.
    // "ramp"      3-6 dB attack, 1 dB release on a dwell.
    // "probe"     probingReleaseConfig(): 6 dB both ways; failed probes double the
    //             interval. What "bandscope" degenerates to without the measurement.
    // "binary"    binaryHighLowConfig(): two-state per-band switch.
    // Same state machine, different numbers; selecting a mode installs its floor.
    // "default" names the law a backend starts with. Neither law nor floor is
    // persisted: every connect reinstalls the default (applyRestoredState).
    // Returns false, changing nothing, on an unknown name.
    bool setAutoRfGainMode(const QString& mode);

    // IAutoRfGainControl (AutoRfGainControl.h), thin forwarders. autoRfGainControl()
    // answers unconditionally; whether to show it is RadioModel::autoRfGain()'s call.
    IAutoRfGainControl* autoRfGainControl() override { return this; }
    void setArmed(bool on) override { setAutoRfGain(on); }
    [[nodiscard]] bool isArmed() const override { return m_autoRfGainEnabled; }
    [[nodiscard]] QString lastArmRefusalReason() const override
    {
        return m_autoRfGainRefusal;
    }
    void setFloorDb(int floorDb) override { setAutoRfGainFloorDb(floorDb); }
    [[nodiscard]] int floorDb() const override { return autoRfGainFloorDb(); }
    [[nodiscard]] int maxFloorDb() const override { return kAutoRfGainFloorMaxDb; }
    bool setLaw(const QString& name) override { return setAutoRfGainMode(name); }
    [[nodiscard]] QString law() const override { return m_autoGainMode; }
    [[nodiscard]] QStringList laws() const override
    {
        // Order is the recommendation: the first is the default. See setAutoRfGainMode.
        return {QStringLiteral("bandscope"), QStringLiteral("ramp"),
                QStringLiteral("probe"), QStringLiteral("binary")};
    }
    [[nodiscard]] QString autoRfGainMode() const { return m_autoGainMode; }

    // The highest baseline from which the automatic control will arm: the top of the
    // native range, so every baseline the slider offers can arm. Outside it the
    // control refuses rather than clamps (#5395): it never moves the operator's number.
    static constexpr int kAutoRfGainMaxBaselineDb = hl2::kLnaGainMaxDb;

    // dspChains()' gather. Static so it cannot reach m_rx: this runs on the I/O
    // thread and m_rx is GUI-thread-owned (push_back/erase reallocate under a
    // reader, #5401). Public as the read-back test seam.
    static QVariantList gatherDspChains(const std::vector<Hl2RxDsp*>& rxDsps,
                                        Hl2TxDsp* txDsp);
    LinkStats linkStats() const override;

    // Point the stream-free telemetry poller at a radio, independent of
    // connectRadio(): the target case is a radio someone else holds. A null address
    // stops the poller and releases its socket. `heldByOther` is the caller's
    // assertion; telemetryLinkState() also reads the radio's own in-use bit.
    void setTelemetryPollTarget(const QHostAddress& addr, bool heldByOther);
    // Borrowed, never owned. Null: this backend does not drive the service.
    void setTelemetryService(Hl2TelemetryService* svc) { m_telemetryService = svc; }

    // Take the model's offline health source if it is ours. The dynamic_cast inside
    // this family is deliberate: RadioModel stays family-blind (#5554 §2.8). A null
    // or foreign source disables the in-band drive; it is not an error.
    void setOfflineHealthSource(IOfflineHealthSource* src) override
    {
        setTelemetryService(dynamic_cast<Hl2TelemetryService*>(src));
    }

signals:
    // Connect-time progress for the client-side WDSP build; not on the seam because
    // WDSP is this family's alone. `stage` is operator-facing text without a counter;
    // `done`/`total` count receiver WDSP channel opens (TX opens none). Emitted on
    // the GUI thread.
    void dspSetupProgress(const QString& stage, int done, int total);
    void dspSetupFinished();

private:
    friend struct Hl2DspReadbackTestAccess;
    friend struct Hl2PanCreateTestAccess;
    friend struct Hl2PcmTestAccess;
    friend struct Hl2TxGateTestAccess;
    friend struct Hl2UnkeyHoldTestAccess;
    // Delivers one bandscope block through MetisClient's signal and ages the mirror,
    // so converter-row expiry is testable without a radio.
    friend struct Hl2HealthBlockTestAccess;
    friend struct Hl2Cl1ReferenceTestAccess;
    // Fires link edges through MetisClient's signals and seeds the connect
    // baseline, so hl2_auto_gain_law_test reads the installed law without a radio.
    friend struct Hl2AutoGainLawTestAccess;
    void applyKeying(bool key, const TxCoordinator::Operation& operation,
                     const TxCoordinator::Completion& completion, bool cwBreakIn);
    void invalidateTxDspConfiguration();
    // Publish linkStats() on the seam's fixed cadence. Timer-driven so the tick
    // survives the radio going silent, which the heartbeat must detect.
    void publishLinkStats();
    // Re-select the companion filter board's band filter for the current slice
    // frequency. Idempotent and change-gated, so it is safe to call from every
    // path that can move the dial.
    void applyBandFilter(const char* reason);
    // Push the TX frequency to the HL2 IO Board, throttled. Called from
    // applyBandFilter() above its `oc == m_ocFilterByte` early return: the filter
    // byte changes per band, the IO board wants every frequency change.
    void applyIoBoardFrequency();
    // The single point where either throttle edge reaches the wire, so the
    // disconnected guard covers both. Returns false when refused.
    [[nodiscard]] bool sendIoBoardFrequency(quint64 hz);
    // Drop the IO-board schedule on linkDown: armed timer, coalesced value and
    // remembered band all describe a session and must not survive one.
    void resetIoBoardSchedule();
    // Reset every bandscope mirror to "never had one". Called from all three link
    // edges so a snapshot between a drop and the next connect cannot report a dead
    // session's bandscope.
    void resetBandscopeMirrors();
    // Age of the mirrored bandscope block; negative when none has arrived, read as
    // "never observed" by all three readers.
    [[nodiscard]] std::int64_t bandscopeBlockAgeMs() const;
    // Per-band memory (RFC #4603): apply the remembered LNA + drive for freqHz's
    // band and record current values for the band being left. Drive falls back to
    // m_driveDefaultPercent (restored per-profile latch); LNA falls back to
    // hl2::kLnaDefaultGainDb, nothing restored (#5829).
    void applyPerBandStateFor(double freqHz, const char* reason);
    void applyLnaGainDb(int gainDb);   // the one true LNA BASELINE application
    // Push m_lnaGainDb - m_lnaAutoOffsetDb to the register, the dB reference and
    // every pan. Called by both writers of the split (see Hl2GainSplit.h): the
    // baseline path above, and the automatic-offset path below.
    void pushEffectiveLnaGain();
    void rememberCurrentBandState();
    void notifyOperatingStateChanged();

    // The connect's three phases. connectRadio() seeds every member synchronously
    // (hl2_state_restore_test reads currentOperatingState() right after it returns),
    // then beginDspSetup() opens the WDSP channels on the I/O thread without blocking
    // the GUI thread (cold FFTW wisdom costs tens of seconds), and finishDspSetup()
    // resumes on the GUI thread and starts the wire. Every chain is still open before
    // MetisClient::start(), serially: EP2 must not stop (docs/HERMES.md §20.8).
    // finishRateChange() is the GUI-thread half of a rate change, after the build
    // thread has produced new chains and the I/O thread swapped them in; see
    // applyPanBandwidth().
    void finishRateChange(bool ok, quint64 generation, int targetRate,
                          int previousRate,
                          const std::vector<QPointer<Hl2RxDsp>>& covered,
                          std::size_t failedIndex,
                          const std::string& error);


    // Which rate is on the wire (not merely attempted) and which crossing is
    // current. The generation is atomic because the I/O-thread install step checks
    // it last before touching anything live; a superseded crossing installs and
    // publishes nothing. See Hl2RateCommit.h.
    AetherSDR::hl2::RateCommitLedger m_rateLedger {48000};

    void beginDspSetup();
    void armDspSetupWatchdog();
    void onDspSetupWatchdog();

    // State the async build carries across event-loop turns (wire params, RX/TX
    // configs, owning connect). Defined in the .cpp to keep forward declarations.
    struct PendingConnect;
    struct DspSetupResult;
    void finishDspSetup(const DspSetupResult& result);

    std::unique_ptr<PendingConnect> m_pendingConnect;
    // Watches the window between beginDspSetup() returning and finishDspSetup()
    // being posted back — the one stretch of the connect that no other timer
    // covers, because MetisClient's watchdog is armed after it (#5413).
    QTimer* m_dspSetupWatchdog = nullptr;
    // A connect that arrived while m_pendingConnect was still building. Held
    // rather than served inline — see the guard at the top of connectRadio().
    std::unique_ptr<RadioConnectRequest> m_queuedConnect;
    quint64 m_connectGeneration = 0;

    void emitSliceState(int ddc);   // sliceChanged(delta) from a receiver's state
    void emitPanState(int ddc);     // panCenterBandwidthChanged from its NCO + rate
    void emitAllSliceState();
    void emitAllPanState();
    void pushInitialState();
    // Put every receiver's AGC pair where this session should come up: remembered
    // values, else construction defaults (#4909). Called conditionally from
    // connectRadio() only; a same-radio reconnect must not reseed.
    void seedReceiverAgc();
    void defineMeters();

    // Declare/withdraw the S-meter of one receiver above the first. Receiver 0's
    // meter stays in defineMeters() because MeterModel::defineMeter uses the
    // preceding "SLC" definition as context for the TX meters. These are declared at
    // receiver creation, so no meter exists for a receiver that does not.
    void defineSliceLevelMeter(int uiNumber);
    void withdrawSliceLevelMeter(int uiNumber);
    void publishTelemetry(const Hl2Telemetry& t);

    // Drive the poller's LinkState from what the IQ path is doing
    // (Hl2TelemetryCadence.h). Called from publishLinkStats() and connect/disconnect.
    void updateTelemetryPollState();
    // What the IQ path is doing, for both the cadence rule and the health
    // snapshot's attribution row, so they cannot disagree.
    [[nodiscard]] Hl2LinkState telemetryLinkState() const;
    // Clamp 0..100, map onto the drive register, honour the transmit gate.
    // Shared by setTxPower() and setTune() so the mapping exists exactly once.
    void applyDrive(int percent);
    static double temperatureCelsius(int raw);
    // Coupler counts -> watts is AetherSDR::hl2::directionalWatts() in MetisProtocol.
    // Watts -> dBm for the meter seam, floored so 0 W does not become -inf.
    static double wattsToDbm(double watts);

    MetisClient* m_metis = nullptr;
    Hl2TxDsp* m_txDsp = nullptr;
    bool m_connected = false;

    // Stream-free telemetry: reads the radio over the alternate control port, on its
    // own socket, when the EP6 path cannot (another client holds it, our stream
    // stalled, or not connected). BORROWED: the service's lifetime is RadioModel's,
    // because it must answer when this backend does not exist.
    Hl2TelemetryService* m_telemetryService = nullptr;
    // What the picker last said about this radio; meaningful only while
    // disconnected (idle radio vs someone else's session).
    bool m_pollTargetHeldByOther = false;
    // When the mirrored EP6 counter last advanced (not a tick-to-tick diff, which
    // aliases two 1 Hz clocks into a false stall; hl2_link_state_alias_test).
    // Restarted from the mirror, so it is independent of the tick rate. Rule and
    // threshold live in Hl2TelemetryCadence.h.
    QElapsedTimer m_rxAdvanceClock;
    quint64 m_rxPacketsAtLastAdvance = 0;
    // Independent of the link-stats cadence on purpose; see the timer's construction.
    static constexpr int kTelemetryPollStateIntervalMs = 1000;

    // Manual frequency calibration (Hl2FreqCal). Every frequency sent to the radio
    // goes through these two: the correction is one scalar and NCO registers cannot
    // be read back, so a bypassing write site mistunes silently. Receiver::sliceFreqHz
    // and ncoHz stay in true RF; only wire and DSP values are scaled.

    // NCO register value (0x01 TX, 0x02+ RX) for a true-RF frequency.
    [[nodiscard]] std::uint32_t ncoCommandHz(double trueHz) const noexcept;
    // DSP shift that puts sliceTrueHz at baseband, given the true-RF NCO. Rounds
    // the NCO command first and computes the shift against that rounded value,
    // so the register's 1 Hz quantisation cancels instead of leaking into audio.
    [[nodiscard]] double dspShiftHz(double sliceTrueHz, double ncoTrueHz) const noexcept;
    // Re-send every NCO (RX banks + TX) from unchanged true-RF state, so a mid-session
    // calibration change is heard immediately.
    void repushAllFrequencies();
    // Clamp, persist, adopt, re-push — the single path for a calibration change
    // whoever asked for it (setup dialog, automation bridge, connect).
    void applyFreqCalPpb(int ppb, bool persist);
    // Enforce "CL1 on means zero manual ppb" — §4 of
    // docs/architecture/hl2-frequency-calibration.md. Returns true when it had to
    // change something, so a live caller knows to re-push frequencies. Called
    // from applyHardwareOptions() AND from connectRadio(), because the two
    // documents are persisted separately and can disagree on disk.
    bool normalizeCl1Calibration(const char* why);
    // Read the actual pending transport snapshot without starting a socket.
    std::optional<std::pair<bool, std::uint32_t>> pendingCl1ReferenceForTest() const;

    // This radio's calibration and the derived scale. 0 / 1.0 is uncalibrated.
    int m_freqCalPpb = 0;
    double m_freqCalScale = 1.0;
    // The connected radio's MAC, so calibration is stored per crystal. Empty until
    // connectRadio().
    QString m_radioSerial;

    // Which HL2 variant this is, loaded per radio at connect: a bare HL2, an HL2+ and
    // a SquareSDR 2 look identical on the wire and the dither bit means different
    // things on each. See Hl2HardwareOptions.
    Hl2HardwareOptions m_hw;
    // Adopt a new set: persist (or not), push every field that changed to the
    // wire, and re-evaluate the band filter. The single path for a hardware
    // change whoever asked for it (setup dialog, automation bridge, connect).
    void applyHardwareOptions(const Hl2HardwareOptions& next, bool persist);
    // Raise or clear the gateware ATU request, honouring m_hw.atuGateware.
    // Called from the same two places that start and end a TUNE.
    void applyAtuTuneRequest(bool tuning);
    // Hand the mixed speaker feed to the radio's own codec, resampled to the
    // EP2 rate. No-op unless this radio has a codec.
    void forwardSpeakerAudioToCodec(const std::vector<float>& mixed);
    // Carry for the 24 -> 48 kHz doubling in forwardSpeakerAudioToCodec(): the last
    // stereo frame of the previous block, so the boundary sample is interpolated.
    float m_codecLastL = 0.0f;
    float m_codecLastR = 0.0f;
    bool m_codecHavePrev = false;

    // One per running DDC, holding only per-receiver state. Anything the hardware
    // shares (sample rate, LNA gain, filter board) stays in the flat members below.
    struct Receiver {
        // Owns its demod + spectrum chain; created on connect, destroyed on disconnect,
        // lives on the I/O thread.
        Hl2RxDsp* dsp = nullptr;

        // Authoritative RX state (nothing on the wire echoes it). The slice frequency
        // and the NCO are independent: the slice is tuned inside the passband by a WDSP
        // shift, and the NCO moves only when the target would leave the window.
        double sliceFreqHz = 10'000'000.0;   // slice
        double ncoHz       = 10'000'000.0;   // DDC / pan centre

        QString mode = QStringLiteral("USB");
        // Overwritten from defaultPassbandForMode(mode) on the first linkUp of each
        // connect (#4484). These initial values match no mode's passband.
        int filterLowHz = 150;
        int filterHighHz = 3000;
        // Authoritative AGC state, mirroring the DSP defaults in Hl2RxDsp::Config
        // so the first sliceChanged reports what WDSP was actually opened with.
        QString agcMode = QStringLiteral("med");
        int agcThresholdDb = 65;

        // Authoritative noise-blanker state: nothing echoes it, and a rebuilt receiver
        // must be told again. Defaults mirror SliceModel's (off, level 50).
        bool nbOn = false;
        int  nbLevel = 50;

        // The operator's panadapter averaging, held for the same reason: a
        // chain built on reconnect or for an added pan starts at none.
        // panAverage is the operator's 0..100; see averageTimeMsForStep().
        int  panAverage = 0;
        bool panWeightedAverage = false;

        // Authoritative squelch state, for the blanker's reasons: nothing on
        // this radio echoes it and every rebuilt chain opens with it off.
        // Defaults mirror SliceModel's (off, level 20). The mode decides which
        // WDSP stage carries it — WdspChannel::setSquelch() — not this struct.
        bool squelchOn = false;
        int  squelchLevel = 20;

        // Host-side per-slice audio: the HL2 mixes nothing. gain is a linear multiplier
        // from the operator's 0..100; pan is 0=left .. 50=centre .. 100=right (SliceModel).
        bool audioMuted = false;
        float audioGain = 1.0f;
        int audioPanPercent = 50;

        // The IQ rate this chain was built for, or 0. Differs from m_sampleRateHz (the
        // rate being attempted) for the length of a rebuild; finishRateChange()
        // reconciles them on success.
        int configuredRateHz = 0;

        // A pending initial build must not be synchronously reconciled by a rate crossing.
        bool dspBuildInFlight = false;

        // UI numbers are reused. Only the generation stamped for this DSP can complete it.
        quint64 dspBuildGeneration = 0;

        // Per-receiver S-meter ballistics, so one receiver's signal never moves another's
        // needle.

        SMeterSmoother sMeter;
    };

    // CW BFO. Receiver filter cuts and sliceFreqHz are carrier-relative (the marker
    // is where the CW signal is); the demodulator needs the audio domain. These
    // helpers are the only translation, so every DSP push goes through them; for
    // non-CW modes the BFO is zero and they are the identity.

    // Where a signal on the marker comes out, in Hz of audio: +pitch for CWU,
    // -pitch for CWL, 0 otherwise.
    [[nodiscard]] double cwBfoHz(const QString& mode) const noexcept;
    // The receiver's passband in the demodulator's audio domain: carrier-relative
    // cuts slid up (CWU) or down (CWL) onto the pitch.
    [[nodiscard]] std::pair<double, double> dspFilterHz(const Receiver& r) const noexcept;
    // WDSP shift: the slice's offset from the NCO less the BFO, so the marker lands
    // on the pitch.
    [[nodiscard]] double rxShiftHz(const Receiver& r) const noexcept;

    // The operator's CW pitch via setCwPitch(). Defaults to TransmitModel's 600.
    int m_cwPitchHz = 600;
    // GUI THREAD ONLY. The sample path reads m_ioDsps instead; see publishIoDsps().
    std::vector<Receiver> m_rx;
    // Whether the last buildReceivers() carried previous receiver state (an
    // auto-reconnect) rather than building fresh. connectRadio()'s AGC seeding needs
    // the difference.
    bool m_rxCarriedState = false;

    // Manual notches, GUI thread only. Above the seam a notch has a stable id; in
    // WDSP it has a positional index that shifts on deletion. This vector keeps
    // WDSP's order, so the index is the position here. Notches are radio-wide: every
    // receiver gets the same set, and a later receiver is seeded (seedNotches).
    struct NotchRecord {
        int id = 0;
        double centerHz = 0.0;
        double widthHz = 0.0;
        bool active = true;
    };
    std::vector<NotchRecord> m_notches;
    // Never reused, so a stale UI reference cannot address a different notch.
    int m_nextNotchId = 1;
    bool m_notchesEnabled = true;

    // Index of `notchId` in m_notches — which IS its WDSP handle — or -1.
    [[nodiscard]] int notchIndexFor(int notchId) const;
    // Push the whole notch set + tune frequency into a receiver added later.
    void seedNotches(const Receiver& r);
    // Re-point a receiver's notch axis at its current NCO; call wherever ncoHz moves.
    void pushNotchTune(const Receiver& r);
    // Push this receiver's NB state into its chain. Needed wherever a chain is
    // (re)built: a fresh Hl2RxDsp opens with the blanker off.
    void pushNoiseBlanker(const Receiver& r);
    // Same, for the squelch, and needed at the same places for the same reason.
    void pushSquelch(const Receiver& r);
    // Same, for the panadapter averaging (Receiver::panAverage / weighted).
    void pushPanAveraging(const Receiver& r);
    // This receiver's NCO just moved: the averaged bins describe the old
    // frequency axis. Called beside pushNotchTune() at the two retune sites.
    void dropPanAverage(const Receiver& r);

    // I/O THREAD ONLY: the chains the EP6 fan-out feeds, indexed by DDC. Never m_rx,
    // whose push_back/erase can move storage under the fan-out. Rebuilt by
    // publishIoDsps() when the receiver set changes, never per packet.
    std::vector<Hl2RxDsp*> m_ioDsps;

    // The four index spaces, never derived from one another (Hl2Receivers.h). GUI
    // thread only; the signal handlers that use it are queued onto this thread.
    Hl2ReceiverMap m_ids;

    // The receiver whose slice is the TX slice: one transmitter, chosen among the
    // receivers.
    int m_txDdc = 0;

    // The AGC pair the operator last set, on any receiver: what
    // currentOperatingState() persists, since the restore writes every receiver.
    // Empty mode = untouched this session.
    QString m_agcMode;
    int     m_agcThresholdDb = 0;
    // The serial seedReceiverAgc() last ran for. A different radio is seeded; the
    // same radio reconnecting is not (buildReceivers() kept its live AGC). Empty
    // until the first connect.
    QString m_agcSeededSerial;

    // The receiver the operator is working on, separate from m_txDdc. The HL2 has no
    // selected-receiver notion and echoes nothing, so this is the client's single
    // answer to "the active slice" (a Flex arbitrates it with `slice set N active=1`).
    int m_activeDdc = 0;

    [[nodiscard]] Receiver* rx(int ddc);
    [[nodiscard]] const Receiver* rx(int ddc) const;
    // Resolve a seam slice/pan id to a DDC index, or -1. Callers must check;
    // defaulting to receiver 0 would move the wrong panadapter.
    [[nodiscard]] int ddcForSlice(int sliceId) const;
    [[nodiscard]] int ddcForPan(const QString& panId) const;

    // Create/destroy the receiver set. buildReceivers() tears the previous set down
    // first so a reconnect at a different count leaves no orphaned WDSP channels.
    void buildReceivers(int count);
    // Create and wire one receiver's DSP chain at `ddc`. Shared by buildReceivers()
    // and createPanadapter() so both paths wire identically.
    bool openReceiverDsp(int ddc, std::string* error);

    // Snapshot on GUI, mark/swap on I/O, open on the existing DSP build thread.
    // Carry UI number (DDC indices move) plus generation (UI numbers are reused).
    // Derive config here for both initial setup and rate catch-up.
    void startReceiverDspBuild(int uiNumber);
    void finishReceiverDspBuild(int uiNumber, quint64 generation, bool ok,
                                int channelId, int builtRateHz,
                                const std::string& error);
    // Backend-lifetime counter: never reset on reconnect, which also reuses UI ids.
    // releaseReceiverDsps() clears each retiring receiver's stamp before copying it.
    quint64 m_nextDspBuildGeneration = 0;
    // How many receivers this radio may run right now: the board's reported
    // count, capped by the link budget at the current sample rate.
    [[nodiscard]] int receiverCeiling() const;
    // Announce a capability revision only if receiverCeiling() moved (#5594). The
    // ceiling (maxSlices/maxPanadapters) falls on zoom-out: at 384 kHz only 3
    // receivers fit 100BASE-T. Guarded so a zoom drag does not storm announcements.
    void announceReceiverCeilingRevision();
    // Re-evaluate the shared band filter and publish the resulting WIDE state.
    void publishWideState();
    // Destroy the DSP chains but KEEP each receiver's operator-set state. The
    // two have different lifetimes — see buildReceivers().
    void releaseReceiverDsps();
    void tearDownReceivers();

    // Hand the I/O thread a fresh copy of the chains. Call after any change to the
    // receiver set. A copy, not locked access: m_rx is GUI-only and m_ioDsps is
    // I/O-only, so nothing is shared, nothing can deadlock against the
    // BlockingQueuedConnection in createPanadapter(), and TSan has nothing to flag.
    // BLOCKS until the I/O thread has taken the list, because callers destroy chains
    // from the old one on return.
    void publishIoDsps();

    // Withdraw every chain from the sample path and block until taken. Use whenever
    // the receiver set changes shape: a shortened list while the wire still sends
    // the old slot count would feed one receiver another's IQ. Empty is correct
    // whatever the wire sends next.
    void withdrawIoDsps();

    // Shared tail. Takes the list by value so it can never alias m_rx.
    void publishIoDspList(std::vector<Hl2RxDsp*> next);

    // Sum one receiver's audio into the host mix (the HL2 has no on-radio mixer).
    void mixReceiverAudio(int ddc, const std::vector<float>& pcm);

    // Receive-audio hold. applyRxAudioMute() is the only writer of m_rxAudioMuted
    // and the path for every mute, so the mixer gate and demod mute stay in step.
    // pushInitialState()'s link-up loop also queues setAudioMuted per new receiver,
    // and agrees because applyRxAudioMute() runs later in that function.
    void applyRxAudioMute(bool muted);
    // Release the hold, but not before the radio has had time to drop out of
    // transmit. Immediate when the hold is zero. Cancelled by any re-key.
    void releaseRxAudioMuteAfterHold();

    // Per-slice meter name ("SLC:LEVEL" for the first receiver). The suffix on the
    // rest is read back by MeterModel::splitMeterId as the sourceIndex, the only way
    // an index crosses meterUpdate's two-argument signature.
    static QString sliceMeterName(int uiNumber);

    // Meter index for a receiver's "SLC"/"LEVEL". Receiver 0 keeps index 1 (as
    // defineMeters() declares); the rest use a band clear of the fixed 1..9.
    static int sliceLevelMeterIndex(int uiNumber);

    // Mixing scratch: per-receiver samples awaiting peers, and a reused summing
    // buffer (~47 calls/s per receiver).
    std::vector<std::deque<float>> m_mixPending;
    std::vector<float> m_mixAccum;
    // How far ahead the other receivers may get before a starved one is mixed as
    // silence: 2048 samples of interleaved L,R at 24 kHz = 1024 frames, ~43 ms.
    static constexpr std::size_t kMixStarvationSamples = 2048;
    // The DDC rate, which IS the panadapter span (emitPanState). Starts at the
    // narrowest; connectRadio() restores the operator's last span (Hl2Settings).
    // The widest costs ~8x (25.2 vs 3.1 Mbps, 3048 vs 381 packets/s), so it is opted
    // into, never imposed. Radio-wide (0x00[25:24]); bounds the receiver count via
    // kEp6LinkBudgetFraction / maxReceiversAtRate() (4 RX at 384 kHz is ~89 Mbit/s
    // on 100BASE-T, refused).
    int m_sampleRateHz = 48000;
    // Receivers to run: operator request (Hl2Settings), clamped by discovery byte
    // 0x13 and the link budget. Never hardcoded: skimmer gateware reports 9..12,
    // hl2b5up_main reports 4.
    int m_requestedNumRx = 1;
    // The board's count (discovery byte 0x13), or 0 from a short reply. Mirrored
    // here because createPanadapter() answers on this thread.
    int m_boardMaxRx = 0;
    // Guards capabilitiesChanged() against a zoom sweep (#5594); the decision is in
    // Hl2CapabilityAnnouncer.h.
    ReceiverCeilingAnnouncer m_ceilingAnnouncer;
    // Assumed when the board never reported its count. hl2b5up_main is built with
    // NR=4 (variants/hl2b5up_main/hermeslite.v). Erring high would stream all-zero
    // slots that look like a dead antenna.
    static constexpr int kAssumedBoardMaxRx = 4;
    // Zoom-sweep throttle for setPanBandwidth (#4470). A span change is a blocking
    // WDSP rebuild plus a settings write, and a drag crosses every intermediate rate.
    // Leading edge applies at once; later requests inside the cooldown coalesce and
    // the last applies on expiry.
    static constexpr int kBandwidthThrottleMs = 150;
    QTimer* m_bandwidthThrottle = nullptr;
    double m_pendingBandwidthHz = 0.0;   // 0 = nothing coalesced

    // The IO board's README asks for at most one frequency update per 0.5 s, only
    // on change. Leading edge queues at once; requests inside the cooldown coalesce
    // and the LAST applies on expiry, never dropped, so the amplifier is not left on
    // the old band. Delivery and relay settling are unacknowledged: this is not an
    // amplifier-ready interlock.
    static constexpr int kIoBoardThrottleMs = 500;
    QTimer* m_ioBoardThrottle = nullptr;
    hl2::IoBoardSchedule m_ioBoardSchedule;
    // The band the IO board was last told (bandKeyForHz()). Empty = nothing told this
    // session, so the first push after connect takes the leading edge. Kept separate
    // from m_currentBandKey, which follows tuning for LNA/drive recall.
    QString m_ioBoardBandKey;

    // Whether this connect already derived the passband from the mode (#4484).
    // pushInitialState() runs on every linkUp, including MetisClient's re-emit after
    // an EP6 silence timeout, which must not reset the operator's filter. Cleared in
    // connectRadio(). Radio-wide: it gates the derivation pass over all receivers.
    bool m_passbandDerivedThisConnect = false;

    // Shared hardware: the HL2 has ONE AD9866, so these are radio-wide.
    // m_lnaGainDb is the operator's BASELINE, not the register (Hl2GainSplit.h): the
    // AD9866 gets this minus m_lnaAutoOffsetDb. Written only by setPanRfGain, the
    // band-memory restore and the connect seed, never by an automatic control.
    int m_lnaGainDb = hl2::kLnaDefaultGainDb;
    // Automatic attenuation below the baseline, dB, never negative. Session state:
    // absent from currentOperatingState() and m_lnaDbByBand so it cannot persist as
    // a chosen gain. Reset to 0 by resetPersistedState().
    int m_lnaAutoOffsetDb = 0;
    // Automatic control (Hl2AutoGainPolicy.h). This flag is "the loop is RUNNING";
    // m_autoRfGainWanted is the operator's wish, OFF by default (no `autoEnabled` key
    // reads false). The shipped +20 dB LNA default is armable; default-on is a
    // separate decision (#5535). No timer: the policy steps on the telemetry publish,
    // so when the stream stops the offset holds; silence is not a clean converter.
    bool m_autoRfGainEnabled = false;
    // The operator's preference, persisted in currentOperatingState()'s rfGain
    // object (family state, per docs/HERMES.md). Stays true when arming is declined,
    // so the next connect from a trusted baseline arms without asking again.
    bool m_autoRfGainWanted = false;
    // Why the last arm attempt was declined; empty otherwise, cleared on success.
    QString m_autoRfGainRefusal;
    AetherSDR::hl2::AutoGainState m_autoGainState;
    AetherSDR::hl2::AutoGainConfig m_autoGainConfig;

    // The loop's visible indicator (#5535 requires it; FrontEndOverload.h).
    // Published only on change, since the inputs move at 10 Hz.
    void publishFrontEndOverload();
    AetherSDR::FrontEndOverload m_lastFrontEndOverload;
    // The configuration's name, for the health row and reset path (the config struct
    // is just numbers). No initialiser: installDefaultAutoGainLaw() sets name and
    // config together, so there is one copy of the default.
    QString m_autoGainMode;
    // A law is a name and its numbers, kept as one value so neither is installed
    // without the other.
    struct AutoGainLaw {
        QString name;
        AetherSDR::hl2::AutoGainConfig config;
    };
    // The law a backend starts with: the one source for the constructor,
    // applyRestoredState() and the name "default".
    [[nodiscard]] static AutoGainLaw defaultAutoGainLaw();
    void installDefaultAutoGainLaw();
    // Band and baseline as the loop last saw them, so changes reach the policy as
    // inputs.
    QString m_autoGainBandKey;
    int m_autoGainBaselineDb = 0;
    int m_autoGainSampleRateHz = 0;
    AetherSDR::hl2::AutoGainReason m_autoGainReason =
        AetherSDR::hl2::AutoGainReason::Disarmed;
    // Restarted on every unkey. The policy's post-unkey hold-off is measured
    // from here; invalid means "not keyed since this control was armed".
    QElapsedTimer m_sinceUnkey;
    void stepAutoGain(const Hl2Telemetry& t);
    // Arm or release the bandscope gate for a law that needs the wideband
    // headroom reading. See the definition for the ownership rule.
    void applyBandscopeForAutoGain();
    // True only when the automatic control started the bandscope; disarming must not
    // stop an operator's own `bandscope.enable`.
    bool m_bandscopeOwnedByAutoGain = false;
    // Last J16 open-collector filter byte. 0xFF = nothing sent yet; kOcNone (0x00)
    // is a real value (all relays released).
    int m_ocFilterByte = 0xFF;
    // Owns the LNA gain <-> dBm coupling so a gain change cannot move the trace.
    Hl2DbReference m_dbRef;

    // Wire and DSP thread, off the GUI thread (see MetisClient.h for EP2 pacing).
    // Owned; joined in the destructor.
    QThread* m_ioThread = nullptr;

    // Rate-change build thread. m_ioThread carries the EP2 2 ms pacer, the EP6 drain
    // and every Hl2RxDsp::processIqBlock() via DirectConnection, so a rebuild there
    // would starve audio and stop EP2, which the gateware watchdog answers by halting
    // the stream (docs/HERMES.md §20.8). m_dspBuildContext owns no state; it is the
    // invokeMethod target living on that thread, parentless because moveToThread()
    // refuses a parented object. Same shape as AnanBackend.
    QThread* m_dspBuildThread = nullptr;
    QObject* m_dspBuildContext = nullptr;

    // Process-wide TX availability, decided at construction: interactive runs may
    // transmit; automation defers to AETHER_AUTOMATION_ALLOW_TX. Mirrored into
    // MetisClient, which refuses independently at the wire.
    bool m_txAllowed = false;
    Hl2Telemetry m_telemetry;
    // Cumulative EP6 sequence gaps, mirrored here from MetisClient::dropsUpdated
    // because MetisClient lives on the I/O thread and healthSnapshot() runs on GUI.
    quint64 m_drops = 0;
    // Bandscope (EP4) counters, mirrored the same way. Not on LinkStats: EP4 is
    // HL2-only, so these reach only this backend's healthSnapshot() rows.
    quint64 m_ep4Packets = 0;
    quint64 m_ep4Drops = 0;
    quint64 m_ep4Rewinds = 0;
    quint64 m_ep4Blocks = 0;
    quint64 m_ep4Timeouts = 0;
    // EP6 silence-watchdog recovery counters, mirrored the same way. Link state, not
    // bandscope: resetBandscopeMirrors() leaves them; MetisClient zeroes them at start().
    quint64 m_silenceRecoveryAttempts = 0;
    quint64 m_silenceRecoveriesCompleted = 0;
    // The bandscope gate's state as MetisClient reports it, never our own request.
    bool m_bandscopeEnabled = false;
    // The outstanding `bandscope.frame` requestId, or 0. One at a time: a reply is
    // one 2048-sample snapshot.
    quint64 m_bandscopeFrameRequest = 0;
    // The most recent accepted bandscope block, mirrored from
    // MetisClient::bandscopeBlockReady. DISPLAY ONLY (IRadioBackend.h); levels are
    // uncalibrated and pre-DDC, comparable only with the gateware clip flag.
    // `samples == 0` = none seen, so the rows stay absent ("absent means not reported").
    AetherSDR::hl2::Ep4Stats m_bandscopeBlock;
    // When that block arrived. Invalid until the first one does.
    QElapsedTimer m_bandscopeBlockClock;
    // Transport counters mirrored from MetisClient::linkCountersUpdated, held as the
    // seam type: MetisClient is only forward-declared here, and translating in the
    // receive lambda keeps the mapping in one place.
    LinkStats m_link;
    QTimer* m_linkStatsTimer = nullptr;
    // rxPackets at the previous tick; the difference answers "is the radio sending".
    quint64 m_linkRxPacketsAtLastTick = 0;
    static constexpr int kLinkStatsIntervalMs = 1000;
    bool m_adcOverload = false;
    // The overload bit is a per-frame comparator sample that dithers on a strong
    // band. telemetryUpdated is coalesced to 10 Hz (#4449), so the edge gate is
    // followed by a rate limit: warn on the first transition, then once per window
    // with the count of assertions seen at 10 Hz (not comparator edges).
    QElapsedTimer m_adcOverloadClock;
    int m_adcOverloadAssertions = 0;
    static constexpr qint64 kAdcOverloadWarnIntervalMs = 10000;

    // Clip rate with its denominator (see Hl2Telemetry): the per-window pair from
    // MetisClient plus session totals, published in healthSnapshot() and driving
    // nothing. The gateware clears the counter only in the EP6 response cycle, so
    // these stop when the stream stops; m_adcWindowClock says how old the last real
    // observation is (stale zero is not a quiet band).
    int m_adcWindowSamples = 0;
    int m_adcOverloadWindowSamples = 0;
    int m_adcWindowMs = 0;
    quint64 m_adcTotalSamples = 0;
    quint64 m_adcTotalOverloadSamples = 0;
    QElapsedTimer m_adcWindowClock;
    // Below this many observations a window has no rate, only a numerator.
    static constexpr int kAdcMinWindowSamples = 4;
    bool m_keyed = false;
    bool m_tuning = false;
    TxCoordinator::Operation m_tuneOperation;   // the TUNE carrier's admission
    bool m_cwAutoKeyed = false;
    QTimer* m_cwHangTimer = nullptr;
    TxCoordinator::Operation m_cwHangOperation;
    TxCoordinator::Operation m_lastTxOperation;
    TxCoordinator::Completion m_cwHangCompletion;
    bool m_txMonitor = false;
    // The receive-audio hold flag, read by mixReceiverAudio() and mirrored to every
    // Hl2RxDsp by applyRxAudioMute(). Not (m_keyed && !m_txMonitor): on key-up it
    // stays true for m_unkeyUnmuteHoldMs after MOX-off is queued (#5497).
    bool m_rxAudioMuted = false;
    // How long the RX mute outlives the unkey, ms (#5497). Measured, not chosen: W,
    // from demod unmute to the last sample of our own TX reaching it, on one HL2
    // (gateware 74.2) into a dummy load: n = 11, median 59.40 ms, range 51.66-66.15;
    // 70 ms covers all 11 with 3.85 ms spare. Every ms is lost receive (#5498:
    // post-unkey dropout 113.74 -> 250.65 ms median), so do not pad it. In CW full
    // break-in the hold is skipped only when the hang is shorter than it (#5850);
    // applyKeying()'s cwBreakIn arm holds the predicate. A member so tests can zero it.
    static constexpr int kUnkeyUnmuteHoldMs = 70;
    int m_unkeyUnmuteHoldMs = kUnkeyUnmuteHoldMs;
    // Single-shot, owned, on this thread, so applyKeying() cancels it without a lock.
    QTimer* m_unkeyUnmuteTimer = nullptr;
    // The flags above flip synchronously while setAudioMuted rides a queued
    // connection, so at key-up they claim "sampling" a block early. This gate answers
    // from the reading's stamp instead; healthSnapshot() feeds it to adcPairing().
    hl2::SliceSamplingGate m_sliceSampling;
    bool m_toneFromTune = false;
    // Last setTxPower() drive, restored after TUNE. Seeded to TransmitModel's
    // rfPower default so an early TUNE restores something sane.
    int m_rfPowerPercent = 100;
    // The applied drive for the health snapshot (#4912): the raw 0..kTxDriveMax value
    // last handed to MetisClient; negative = never written, row absent. The "gated"
    // row is derived from m_txAllowed at read time.
    int m_txDriveRegister = -1;
    // RFC #4603 state memory. m_restoredState is the validated pre-connect snapshot;
    // the per-band maps (Hl2Bands.h keys) are the session's working copies, with
    // defaults for unvisited bands. m_currentBandKey says which band live edits
    // belong to.
    bool m_haveRestoredState = false;
    RestoredRadioState m_restoredState;
    QMap<QString, int> m_lnaDbByBand;
    QMap<QString, int> m_driveByBand;
    // No m_lnaDefaultDb: the LNA fallback is hl2::kLnaDefaultGainDb (#5829), unlike
    // drive's per-profile m_driveDefaultPercent latch.
    // m_lnaSessionPin: the connect param pinned a gain the start band had stored
    // differently; live value honoured, persistence refused (Hl2BandMemoryPolicy.h).
    // Cleared when the operator changes gain or leaves the start band.
    bool m_lnaSessionPin = false;
    int m_driveDefaultPercent = -1;  // <0: no restored default; leave drive alone
    QString m_currentBandKey;
    // True while band-memory/restore code drives setTxPower(): only operator intent
    // bootstraps the baseline or records into the per-band map.
    bool m_applyingBandMemory = false;

    // The operator's TX passband and whether they have set one.
    // defaultTxPassbandForMode() is re-pushed on every mode set and TX-slice move
    // and cannot tell "unchosen" from "chose 300..2700"; the flag keeps an operator
    // passband (e.g. eSSB) across mode changes.
    bool m_txFilterFromOperator = false;
    int m_txFilterLowHz = 300;
    int m_txFilterHighHz = 2700;

    // Loudest mic peak of this transmission, dBFS, for the unkey "went out quiet"
    // diagnostic (the ALC only reduces). -140 is Hl2TxDsp::micPeak's silence floor
    // = nothing measured.
    float m_txMicPeakMaxDbfs = -140.0f;

    // True once this transmission carried client-leveled (TCI/DAX) audio. Gates the
    // unkey mic-gain diagnostic off and is reported in healthSnapshot(). Set in
    // submitTxAudio(), cleared on each key edge in setKeying().
    bool m_txAudioClientLeveled = false;
    // True once this transmission carried EngineGenerated audio (the WSPR pump).
    // Gates the unkey mic-gain diagnostic off: no mic slider is in that path. The
    // AX.25 modem is tagged Microphone, since the slider is its only control.
    bool m_txAudioEngineGenerated = false;

    // The passband to push at the modulator for `mode`: the operator's if they
    // have chosen one, otherwise that mode's default.
    std::pair<int, int> effectiveTxPassband(const QString& mode) const;
    // Apply that passband and announce it as a TransmitDelta, so the applet shows
    // what the transmitter runs. setTxFilter() is the one push that bypasses this;
    // see the definition.
    void pushTxPassband(const QString& mode);
    // Tune-carrier amplitude, full scale. Radiated power is set by the TX drive
    // register; scaling here too would make the power control non-linear.
    static constexpr double kTuneCarrierAmplitude = 1.0;
    int m_lastFwdRaw = -1;

    // Meter ballistics: the S-meter's rate gate and EMA are SMeterSmoother's
    // (WdspSMeter.h), shared with AnanBackend (100 ms tick, attack 0.5, decay 0.15),
    // fed ~47 readings/s at any sample rate. Indices 1..9 are defineMeters()' fixed
    // catalogue; receivers above the first take kSliceLevelMeterBase + uiNumber.
    static constexpr int kSliceLevelMeterBase = 100;
    // S-meter state is per receiver (Receiver::sMeter). PA temperature rides the
    // 10 Hz telemetry and needs only a symmetric EMA against ADC low-bit flicker.
    static constexpr double kPaTempAlpha = 0.2;
    double m_paTempC = 0.0;
    bool   m_havePaTemp = false;

    // Forward-power peak hold: a PEP ESTIMATE. The HL2 has no peak detector: forward
    // power is one 12-bit slow_adc I2C conversion, round-robined with reverse power,
    // temperature and bias (rtl/slow_adc.v, rtl/control.v ~L262), seen every
    // kTelemetryMinIntervalMs. 10 Hz sampling misses speech peaks (SSB read 8-12 dB
    // below PEP), so the max is held across the over: instant attack, release 0.05
    // per 10 Hz sample (~2 s, like an outboard PEP meter), in WATTS because the
    // calibration curve is non-linear. Raw counts are still logged and published.
    // TxApplet's own PEP tick (#2561) therefore tracks the gauge fill on an HL2.
    static constexpr double kFwdPeakReleaseAlpha = 0.05;
    double m_fwdPeakWatts = 0.0;

    // Operator's MIC slider position, 0..100 (50 = unity; see setMicGain()), kept
    // for reporting alongside the linear gain it maps to.
    int m_micLevel = 50;

    // Mic level from this radio's restored document, applied once by
    // pushInitialState() (m_txDsp does not exist at applyRestoredState() time).
    // -1 = nothing stored or already consumed; not 0, which is the slider's MUTE
    // (hl2::micSliderToLinear).
    int m_restoredMicLevel = -1;

    // Voice-chain mirrors for healthSnapshot(): originate on the DSP worker and are
    // mirrored on signal delivery, so the GUI thread reads local values and can
    // answer after the over. Also published as meters. NaN = never reported (0 dB
    // of ALC gain is a real state).
    double m_alcGainDb = std::numeric_limits<double>::quiet_NaN();
    double m_alcPeakDbfs = std::numeric_limits<double>::quiet_NaN();
    // The modulator's linear mic gain as echoed by Hl2TxDsp. NaN until confirmed, so
    // "never landed" differs from "landed at unity".
    double m_appliedMicGainLinear = std::numeric_limits<double>::quiet_NaN();

    // The ALC target peak connectRadio() configured, read by healthSnapshot() and
    // setKeying()'s unkey mic diagnostic. The ALC only reduces, so the mic peak is
    // the on-air level up to this target. Seeded with Config's default as a literal
    // (Hl2TxDsp is forward-declared); a static_assert in Hl2Backend.cpp pins them.
    double m_alcTargetPeak = 0.85;

    // The AGC threshold -> WDSP gain ceiling map is
    // Hl2DbReference::kAgcCeilingDbPerUnit via m_dbRef.agcCeilingDb(), so the
    // ceiling is referred to LNA gain like the displayed dBm.

    // Fraction of the half-span the slice may occupy before the NCO re-centres.
    // 0.8 leaves the outer 20% of each side for filter roll-off.
    static constexpr double kUsablePassbandFraction = 0.8;
    // Ceiling on host-mixed slice audio. N demodulated receivers are summed
    // here, so N loud slices can sum past full scale where one never could.
    static constexpr float kMixCeiling = 1.0f;
    // Centre of SliceModel's 0..100 balance range.
    static constexpr int kAudioPanCentre = 50;

    // AD9866 LNA gain limits, dB: the range ccRxGain() encodes (C4 = 0x40 | (dB + 12),
    // 6-bit), so clamping elsewhere would truncate silently on the wire.
    static constexpr int kLnaGainMinDb  = hl2::kLnaGainMinDb;
    static constexpr int kLnaGainMaxDb  = hl2::kLnaGainMaxDb;
    static constexpr int kLnaGainStepDb = hl2::kLnaGainStepDb;

    // TX passband ceiling: Nyquist of the 24 kHz TX AUDIO rate (AudioEngine), not
    // the 48 kHz EP2 rate. Shared by setTxFilter() and applyRestoredState() so a
    // restore admits exactly what the setter can produce.
    static constexpr int kTxAudioMaxHz = 12000;
};

}  // namespace AetherSDR::hl2
