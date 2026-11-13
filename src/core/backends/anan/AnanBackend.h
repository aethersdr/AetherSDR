#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/AnanSliceAudio.h"
#include "core/Resampler.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/AnanReceiverRoutes.h"
#include "core/backends/anan/P2Client.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QMap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR::anan {

// IRadioBackend for the ANAN-G2 (openHPSDR Protocol 2). Owns one P2Client and,
// per receiver, one AnanRxDsp, all on a dedicated I/O thread. A receiver is a
// slice, the pan that shows its DDC, and the DSP behind them (Receiver below);
// kReceiverRoutes says how many there are and where each sits. RX only.
//   - No NCO/WDSP-shift split: a receiver's DDC retunes on every
//     setSliceFrequency()/setPanCenter(), so "the shift is ALWAYS exactly
//     -cwBfoHz(mode), because the NCO IS the slice frequency".
//   - capabilities()' identity strings are fixed; gateware, DDC count and board
//     id come from this session's Discovery reply once it lands.
//   - setKeying() is a no-op: canTransmit is false and P2Client has no PTT
//     (TX is RFC §2.11 Phase 3).
// Receive handedness lives entirely in AnanRxDsp (see docs/HERMES.md §16).
class AnanBackend : public IRadioBackend {
    Q_OBJECT

public:
    // Public for testing. The seeded defaults are reported here alongside a
    // sweep's measured tables, so the one invariant worth pinning is that a
    // rate claims NO correction until connectRadio() has actually seeded one
    // -- reporting a default as live on a disconnected radio would be the
    // same lie in the other direction as the "nothing measured" this replaced.
    // See anan_backend_test.
    [[nodiscard]] QVariantMap droopStatus() const;

    explicit AnanBackend(QObject* parent = nullptr);
    ~AnanBackend() override;

    RadioCapabilities capabilities() const override;
    bool ownsRxAudio() const override { return true; }

    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& requested) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    // The AGC-off level, which the base class drops. Mode and threshold edits
    // go to setSliceAgc() as before.
    void requestSliceAgc(int sliceId, const SliceAgcRequest& request) override;
    void setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                              int level, AetherSDR::NoiseBlankerFill fill) override;
    ReceiveDispatch requestSliceDsp(int sliceId, const SliceDspRequest& request) override;
    // RIT, from the desktop (SliceModel::setRit) and from slice.setRit, reaches
    // setSliceRit() here; XIT stays unsupported.
    ReceiveDispatch requestSliceControl(int sliceId, const SliceControlRequest& request) override;
    // RIT moves the detector's zero (the WDSP shift) by the offset; the DDC and
    // panadapter stay on the slice frequency. Enable and offset as one intent,
    // clamped to kMaxRitOffsetHz; requestSliceControl(Rit) is how both the
    // desktop and the headless slice.setRit reach it.
    void setSliceRit(int sliceId, bool on, int hz);
    ReceiveDispatch requestSliceAudio(int sliceId, const SliceAudioRequest& request) override;

    // The receiver's own audio stage. Without these three the operator's mute,
    // AF fader and balance moved and nothing happened: IRadioBackend's defaults
    // are no-ops, and a backend that demodulates on this host is the only thing
    // in the chain that can apply them (see AnanSliceAudio.h).
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;

    // The RADIO's own output level and mute, applied to the speaker stream only.
    //
    // Separate from the per-slice stage above, and both apply: the slice stage is
    // how loud this receiver is wherever it is heard, this is how loud the RADIO
    // plays. Without it the radio gets the samples at whatever the slice stage
    // left them -- full scale by default -- and the G2 has no speaker volume
    // register to turn that down with.
    void setLineoutGain(int percent) override;
    void setLineoutMute(bool mute) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    // The G2's receive step attenuator, presented as RF gain: -31..0 dB,
    // where -12 means 12 dB of attenuation. See the definition.
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;
    void setPanPixelWidth(const QString& panId, int pixels) override;
    void setCwPitch(int hz) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;
    // Transport snapshot for the status bar's Network field and Network
    // Diagnostics. Synchronous, and reads only the cache onLinkCounters() keeps
    // on this thread -- never the client's own counters, which belong to the
    // I/O thread. The push half is onLinkCounters(), on the client's tick.
    [[nodiscard]] LinkStats linkStats() const override;

    // ---- pure-function pieces, exposed static for testability ----
    // (matches the WdspChannel/Hl2RxDsp precedent of exposing normally-
    // internal helpers specifically so a test can pin them directly, rather
    // than only indirectly through the class's live behaviour.)

    // Mode string -> WdspChannel::Mode. Unknown strings fall back to USB
    // (matches Hl2Backend's own modeFromString fallback) -- HERMES.md §16.7's
    // own regression: "CW" (not just "CWU") and "NFM" both need entries or a
    // radio reporting either falls through silently and demodulates as SSB.
    [[nodiscard]] static WdspChannel::Mode modeFromString(const QString& mode) noexcept;

    // Per-mode default passband, operator-facing (marker-relative) Hz.
    // Vocabulary matches modeFromString's.
    [[nodiscard]] static std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept;

    // Every spelling modeFromString() maps, case-insensitive. The restore needs
    // it because modeFromString() falls back to USB instead of failing.
    [[nodiscard]] static bool isKnownModeString(const QString& mode) noexcept;

    // The modes the mode menu offers, each once. setSliceMode() and the restore
    // take only these and their aliases: WBFM (the WDSP channel runs at 48 kHz,
    // too narrow for broadcast FM) and DRM (no decoder) are refused, since a
    // slice left in a mode no record offers refuses every headless verb.
    [[nodiscard]] static const QStringList& publishedModeStrings() noexcept;
    // A spelling setSliceMode() and the restore accept: an offered mode or its alias.
    [[nodiscard]] static bool isOfferedModeString(const QString& mode) noexcept;

    // The offered spelling of an accepted one: uppercased, with each alias
    // collapsed onto its offered twin. Display-only; the DSP mode is unchanged.
    [[nodiscard]] static QString canonicalOfferedMode(const QString& mode) noexcept;

    // Where a session starts when this radio has no remembered state.
    static constexpr double kFirstConnectFrequencyHz = 14'175'000.0;
    static constexpr const char* kFirstConnectMode = "USB";

    // Bounded by the DDS encoding, not a band edge: tuningMinHz/MaxHz are not
    // reported. Refused, never clamped, at or above Nyquist.
    [[nodiscard]] static bool isRestorableFrequencyHz(double hz) noexcept;

    // Headless slice.setFrequency bounds (sliceFrequencyControl): the measured
    // tuning span, both edges inside isRestorableFrequencyHz(). See capabilities().
    static constexpr qint64 kHeadlessTuneMinHz = 10'000;
    static constexpr qint64 kHeadlessTuneMaxHz = 61'430'000;
    static_assert(0 < kHeadlessTuneMinHz && kHeadlessTuneMinHz <= kHeadlessTuneMaxHz
                      && kHeadlessTuneMaxHz < static_cast<qint64>(kDspClockHz) / 2,
                  "headless tuning bounds must sit inside (0, Nyquist)");

    // Largest |RIT offset|. The widest passband edge is 12 kHz in every mode (a
    // 12 kHz cut, or CW's 6 kHz cut plus its 6 kHz BFO cap), so the shifted edge
    // stays inside the WDSP channel's +-24 kHz at its 48 kHz DSP rate.
    static constexpr int kMaxRitOffsetHz = 9'999;
    // WDSP ANR's level before the operator sets one, and when none is restored.
    static constexpr int kDefaultNrLevel = 50;

    // The CW BFO offset (HERMES.md §5: "CW has no BFO unless you build one").
    // +pitchHz for CWU/CW, -pitchHz for CWL, 0 for every other mode -- zero
    // for non-CW is why every mode routes through this rather than only the
    // CW ones (entering/leaving CW must re-push the shift either way).
    [[nodiscard]] static double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept;

    // The WDSP shift for a receiver: RIT moves the detector's zero up by the
    // offset, and the CW BFO pushes it down a pitch (a positive shift raises the
    // receive point; anan_rxdsp_handedness_test pins that direction).
    [[nodiscard]] static double detectorShiftHz(const QString& mode, int pitchHz,
                                                bool ritEnabled, int ritOffsetHz) noexcept
    {
        return (ritEnabled ? static_cast<double>(ritOffsetHz) : 0.0)
               - cwBfoOffsetHz(mode, pitchHz);
    }

    // Snaps an operator-requested span to the nearest rate this radio
    // actually offers (capabilities().sampleRatesHz), by RATIO (log
    // distance), not linear -- see the definition comment for why linear
    // distance is provably wrong for these octave-spaced rates
    // (HERMES.md §15.1; mirrors Hl2Backend::nearestIqSampleRateHz()).
    // There is no continuous zoom here -- the DDC runs at exactly one of six
    // fixed rates.
    [[nodiscard]] static int nearestDdc0RateKsps(int requestedKsps) noexcept;

    // The AGC-T knob (0..100) as WDSP gain in dB: the AGC's maximum gain, or
    // the fixed gain with AGC off. -20..120 dB is the range of deskHPSDR's AGC
    // gain slider and of Thetis's AGC-T, which sets both gains the same way.
    // With AGC on, the AGC reaches its maximum only on weak signals, so a high
    // setting cannot clip a strong one. With AGC off, the knob is the gain.
    static constexpr double kAgcKnobMinDb = -20.0;
    static constexpr double kAgcKnobDbPerStep = 1.4;
    [[nodiscard]] static constexpr double agcKnobDb(int level) noexcept
    {
        return kAgcKnobMinDb + kAgcKnobDbPerStep * std::clamp(level, 0, 100);
    }
    // 71 = 79.4 dB, the step nearest deskHPSDR's 80 dB AGC gain default.
    static constexpr int kDefaultAgcThreshold = 71;
    // 29 = 20.6 dB, the step nearest Thetis's 20 dB fixed gain default
    // (deskHPSDR has no AGC-off gain control).
    static constexpr int kDefaultAgcOffLevel = 29;

    // RX1's live operator AGC state as setSliceAgc()/requestSliceAgc() last
    // stored it, which connectRadio() and beginRateChange() build the DSP
    // config from. Exposed read-only for testing without a live radio (matches
    // WdspChannel's own *ForTest accessor convention), not part of the
    // operator-facing seam.
    [[nodiscard]] int agcModeForTest() const noexcept { return rx1().agcMode; }
    [[nodiscard]] double agcMaxGainDbForTest() const noexcept { return agcKnobDb(rx1().agcThreshold); }
    [[nodiscard]] double agcFixedGainDbForTest() const noexcept { return agcKnobDb(rx1().agcOffLevel); }
    // RX1's live receive state as applyLiveRxState() writes it into the DSP config.
    [[nodiscard]] AnanRxDsp::Config liveRxConfigForTest() const
    {
        AnanRxDsp::Config config;
        applyLiveRxState(rx1(), config);
        return config;
    }
    // The step attenuation of the ADC behind RX1.
    [[nodiscard]] int attenuationDbForTest() const noexcept { return attenuationDbForAdc(rx1().adc); }
    [[nodiscard]] bool noiseBlankerOnForTest() const noexcept
    {
        return rx1().nbKind != AetherSDR::NoiseBlankerKind::Off;
    }
    [[nodiscard]] AetherSDR::NoiseBlankerKind noiseBlankerKindForTest() const noexcept
    {
        return rx1().nbKind;
    }
    [[nodiscard]] int noiseBlankerLevelForTest() const noexcept { return rx1().nbLevel; }
    [[nodiscard]] AetherSDR::NoiseBlankerFill noiseBlankerFillForTest() const noexcept
    {
        return rx1().nbFill;
    }
    // RX1's mode and passband, as the slice setters last stored them.
    [[nodiscard]] QString modeForTest() const { return rx1().mode; }
    [[nodiscard]] std::pair<bool, int> ritForTest() const noexcept
    {
        return {rx1().ritEnabled, rx1().ritOffsetHz};
    }
    [[nodiscard]] std::pair<bool, int> noiseReductionForTest() const noexcept
    {
        return {rx1().nrEnabled, rx1().nrLevel};
    }
    [[nodiscard]] std::pair<int, int> filterForTest() const noexcept
    {
        return {rx1().filterLowHz, rx1().filterHighHz};
    }
    // What the next session asks the radio for, built from the receivers
    // exactly as startP2ClientSession() builds it.
    [[nodiscard]] P2Client::Params sessionParamsForTest() const { return sessionParams(); }
    // The radio's own output level/mute as the lineout seam last set them. Same
    // *ForTest convention as the accessors above: read-only, not part of the seam.
    [[nodiscard]] int lineoutGainPercentForTest() const noexcept { return m_lineoutGainPercent; }
    [[nodiscard]] bool lineoutMutedForTest() const noexcept { return m_lineoutMuted; }
    // What applyRestoredState() accepted; connectRadio() consumes it behind a socket.
    [[nodiscard]] double restoredFrequencyHzForTest() const noexcept { return m_restoredFreqHz; }
    [[nodiscard]] QString restoredModeForTest() const { return m_restoredMode; }
    // Drives the S-meter path as AnanRxDsp::meterUpdate would, so the
    // smoothing and publish tick can be tested without a live radio.
    void feedMeterForTest(float dbfs) { onDspMeter(dbfs); }
    // The smoothed value itself, whether or not the publish tick has come
    // round. Without this the ballistics can only be observed through the
    // tick, which makes the assertion depend on wall-clock timing and lets a
    // test pass with the smoothing replaced by a plain assignment.
    [[nodiscard]] double sMeterDbmForTest() const noexcept { return m_sMeter.value(); }

private:
    friend class AnanNoiseBlankerTestAccess;
    friend class AnanLinkStatsTestAccess;
    friend class AnanSupplyRailTestAccess;

    // One receiver: the slice the operator tunes, the pan that shows its DDC,
    // and the DSP between them. m_receivers is indexed like kReceiverRoutes:
    // the receiver at index i owns slice i, pan "anan-i" and spectrum pan index
    // i, and RX1 is index 0. Everything here lives on this object's thread
    // except the AnanRxDsp, which lives on m_ioThread and is reached only by
    // queued calls (or DirectConnection from P2Client on that same thread).
    struct Receiver {
        int index = 0;
        ReceiverRoute route;
        // The ADC this receiver listens to: the route's, unless the operator
        // picked the other input (anan.ddc0AdcIndex, which only RX1 reads).
        int adc = 0;
        QString panId;
        AnanRxDsp* dsp = nullptr;   // nullptr parent: moveToThread requires it
        // The DDC's rate, and the DSP config a connect or a rate change builds
        // from; inputSampleRateHz carries the same rate in Hz.
        int rateKsps = 48;
        AnanRxDsp::Config pendingDspConfig;
        // Panadapter points from the panel width (setPanPixelWidth()), kept here
        // as well as in the DSP so a connect after the width arrives still
        // builds the analyzer at that count.
        int panPoints = static_cast<int>(kDroopCorrectionFftSize);
        // The operator's receive state. connectRadio() and beginRateChange()
        // build the DSP config from it and emitSliceState() publishes it, so it
        // is retained across rate changes and reconnects.
        QString mode = QStringLiteral("USB");
        int filterLowHz = 100;
        int filterHighHz = 2900;
        double freqHz = 0.0;
        // A retune the throttle still owes the radio and the pan
        // (scheduleTuneApply()).
        bool tuneDirty = false;
        // AGC in AGC-T knob units (agcKnobDb() converts). applyRestoredState()
        // sets RX1's before every connect from this radio's stored AGC (the Agc
        // domain), or the defaults when it has none, so another ANAN does not
        // inherit it the way it inherits the noise blanker below.
        int agcMode = 3;
        int agcThreshold = kDefaultAgcThreshold;
        int agcOffLevel = kDefaultAgcOffLevel;
        // Noise blanker as setSliceNoiseBlanker() last stored it. emitSliceState()
        // also publishes it when a different radio gets a fresh slice, keeping
        // its NB button in agreement. Defaults match AnanRxDsp::Config's.
        AetherSDR::NoiseBlankerKind nbKind = AetherSDR::NoiseBlankerKind::Off;
        int nbLevel = 50;
        AetherSDR::NoiseBlankerFill nbFill = AetherSDR::kDefaultNoiseBlankerFill;
        // WDSP ANR and RIT, each echoed by emitSliceState().
        bool nrEnabled = false;
        int nrLevel = kDefaultNrLevel;
        bool ritEnabled = false;
        int ritOffsetHz = 0;
        // The receiver's audio stage (AnanSliceAudio.h). 100/50 = unity, centered.
        bool audioMuted = false;
        int audioGainPercent = 100;
        int audioPanPercent = 50;
    };
    std::array<Receiver, kReceiverRoutes.size()> m_receivers;
    [[nodiscard]] Receiver& rx1() noexcept { return m_receivers.front(); }
    [[nodiscard]] const Receiver& rx1() const noexcept { return m_receivers.front(); }
    // The receiver a seam call addresses, or nullptr for a slice or pan this
    // backend does not have; such a call changes nothing.
    [[nodiscard]] Receiver* receiverForSlice(int sliceId) noexcept;
    [[nodiscard]] Receiver* receiverForPan(const QString& panId) noexcept;
    // The step attenuation in front of one ADC (0 or 1), as m_pendingParams
    // holds it for the session. Every receiver on that ADC shares it.
    [[nodiscard]] int attenuationDbForAdc(int adc) const noexcept
    {
        return adc == 1 ? m_pendingParams.adc1AttenuationDb
                        : m_pendingParams.adc0AttenuationDb;
    }
    // m_pendingParams with the session's DDC list built from the receivers,
    // RX1 first (P2Client::Params::activeDdcs: the first entry is the link DDC).
    [[nodiscard]] P2Client::Params sessionParams() const;

    // A receiver's DSP outputs (audio, spectrum) to this object, on its own
    // slice and pan. Called once per receiver from the constructor.
    void connectReceiverOutputs(int index);

    void beginDspSetup();
    void finishDspSetup(quint64 generation, bool ok, const QString& error);
    // beginDspSetup() builds one channel per receiver; finishDspSetup() starts
    // the session once the last of them is in. The first failure is kept.
    int m_dspSetupPending = 0;
    QString m_dspSetupError;
    // The "restart P2Client with m_pendingParams, then retune" half of what
    // finishDspSetup() used to do inline -- extracted so beginRateChange()'s
    // background-rebuild path (finishRateChange()) can reuse it too, instead
    // of a second copy. Reads m_rateChanging to decide which connect timeout
    // to pass (see P2Client::start()'s own comment).
    void startP2ClientSession(quint64 generation);
    // Set by connectRadio() just before beginDspSetup(); read back by
    // finishDspSetup() once AnanRxDsp::configure() completes, so the P2Client
    // session isn't started until the DSP chain that will consume its IQ
    // actually exists.
    P2Client::Params m_pendingParams;
    void emitSliceState(const Receiver& rx);
    void emitPanState(const Receiver& rx);
    // Every receiver's, for the paths that report the whole radio.
    void emitSliceState();
    void emitPanState();

    // Convert one demodulated block to the radio's speaker stream and hand it to
    // P2Client. Takes the SAME buffer the speakers get, after the receiver's
    // audio stage, so mute and the fader reach the radio as well.
    //
    // Does nothing unless the session was started with speaker audio enabled.
    void sendSpeakerAudioToRadio(const QByteArray& stereoFloat);
    // Build or discard the two resamplers for the current audio rate. Called
    // whenever the DSP is configured or rebuilt, because the rate is theirs to
    // follow and a rate change invalidates their filter state.
    void resetSpeakerResamplers();
    // Declares SLC:LEVEL and RAD:+13.8A to the meter seam; on every connect,
    // before the first reading can arrive. See its definition.
    void defineMeters();
    // One client snapshot -> one LinkStats push. The FIXED cadence is the
    // client's timer, not ours: this runs on whatever the client published, and
    // it must keep arriving after the radio goes quiet, because "nothing came
    // this second" is the observation the heartbeat's alarm path waits for
    // (IRadioBackend::LinkStats).
    void onLinkCounters(const P2Client::LinkCounters& counters);
    // The last snapshot the client published, cached on THIS thread so the
    // synchronous linkStats() getter never reads the I/O thread's counters --
    // see P2Client::LinkCounters for why that read would be a race.
    P2Client::LinkCounters m_linkCounters;
    bool m_linkCountersSeen = false;
    // rxBytes at the previous snapshot, for LinkStats::alive -- a link that is
    // bound and counting but has gone quiet must read as dead, which a
    // cumulative total alone cannot say.
    quint64 m_lastSnapshotRxBytes = 0;
    // One WDSP S-meter reading (dBFS) -> dBm, smoothed, published on a tick.
    // RX1's: SLC:LEVEL is the one receive meter defineMeters() declares.
    void onDspMeter(float dbfs);
    // The same smoother Hl2Backend publishes through, so the two receivers'
    // needles move alike by construction -- see WdspSMeter.h.
    SMeterSmoother m_sMeter;
    // One status packet's supply-rail count -> RAD:+13.8A, in volts, as the MEAN
    // of every count since the last publication, once a second. A plain mean
    // and not the S-meter's attack/decay: the readout wants the rail's level,
    // and one packet alone wanders by about +/-11 counts (+/-0.3 V) on the bench.
    void onSupplyRailCounts(int counts);
    // The clocked half of the above, publication included, so the window and
    // the meter it feeds are testable against an injected time.
    void onSupplyRailCountsAt(int counts, qint64 nowMs);
    // Every 1000 ms: under MeterModel::kVitalsFreshMs (1500), so a live rail
    // never reads as stale between publications, and about five packets at
    // p2app's 200 ms receive cadence.
    static constexpr qint64 kSupplyRailWindowMs = 1000;
    // Empties the window. Called on disconnect and at linkUp, so a new session
    // never averages in the last one's counts.
    void resetSupplyRailWindow();
    QElapsedTimer m_supplyRailClock;
    // -1 = no window open.
    qint64 m_supplyRailWindowStartMs = -1;
    qint64 m_supplyRailCountSum = 0;
    int m_supplyRailSamples = 0;
    // Leading+trailing throttle around applyTuneToRadioAndPan() -- see
    // setSliceFrequency()'s comment for why an unthrottled click/drag-tune
    // gesture is a problem for this backend specifically.
    void scheduleTuneApply();
    void applyTuneToRadioAndPan();
    // Live rate change for setPanBandwidth() -- see its own definition
    // comment for why this is a full stop+reconfigure+restart of the
    // P2Client session rather than an in-place resend while streaming.
    // Kicks off the background DSP rebuild (AnanRxDsp::buildChannel(), on
    // m_dspBuildThread) and returns immediately -- the old channel and the
    // live P2Client session both keep running, undisturbed, for the whole
    // build. finishRateChange() is what actually stops/restarts P2Client,
    // once the new channel already exists.
    // One receiver's DDC at a time (m_rateChanging is radio-wide).
    void beginRateChange(int receiverIndex, int newRateKsps);
    // Runs once the background build (started by beginRateChange()) has
    // finished and been installed. Same body as the old beginRateChange()'s
    // second half, just triggered after the rebuild instead of before it --
    // see its own definition comment for the retry-window fix folded in
    // here too.
    void finishRateChange(int receiverIndex, quint64 generation, bool ok,
                          const QString& error);

    // Identity guard for the droop-calibration write, in front of
    // AnanDroopCalibrator::saveTables() (which owns the merge, the schema
    // guard and the codec). Returns an empty string on success, or the
    // operator-facing reason it did not persist -- never void: the caller
    // reports the outcome to the dialog and the bridge, which both used to
    // claim success regardless.
    [[nodiscard]] QString persistDroopTables(
        const QMap<int, anan::DroopCorrectionTable>& tables);
    // If a zoom request arrived while a previous one was still in flight
    // (m_pendingBandwidthKsps != 0), starts it now. Called from every path
    // that clears m_rateChanging -- linkUp success and both finishDspSetup()
    // failure branches -- so a fast zoom sweep converges on the operator's
    // LATEST request instead of stalling on whichever one happened to be
    // running when they stopped clicking.
    void retryPendingRateChange();
    [[nodiscard]] double cwBfoHz(const Receiver& rx) const noexcept
    {
        return cwBfoOffsetHz(rx.mode, m_cwPitchHz);
    }
    // One receiver's mode, passband, AGC and noise blanker, written into a DSP
    // config. connectRadio() and beginRateChange() both build theirs with it.
    void applyLiveRxState(const Receiver& rx, AnanRxDsp::Config& config) const;
    // Re-push mode + filter (with the CW BFO folded in) + shift, in that
    // order, to the receiver's DSP. HERMES.md §16.7: mode changes must re-push
    // the passband, every time, not only when its value changed.
    void pushModeFilterShift(const Receiver& rx);

    QThread* m_ioThread = nullptr;
    P2Client* m_client = nullptr;    // lives on m_ioThread; nullptr parent (moveToThread requires it)
    // Build-only thread for a rate change's background AnanRxDsp::buildChannel()
    // call -- never touches P2Client or the real-time IQ path, so it can never
    // starve the keepalive the way blocking m_ioThread with the same work
    // used to (see beginRateChange()'s comment). m_dspBuildContext is a bare
    // QObject living there, used purely as an invokeMethod thread-affinity
    // target -- it owns no state of its own.
    QThread* m_dspBuildThread = nullptr;
    QObject* m_dspBuildContext = nullptr;
    bool m_connected = false;
    // Bumped on every connectRadio()/disconnectRadio(); a finishDspSetup()
    // callback checks the generation it captured against the current one and
    // bails silently if they differ -- the guard against a slow first
    // AnanRxDsp::configure() (FFTW PATIENT planning, ~19s cold per
    // HERMES.md §22.3) completing after a newer connect or a disconnect.
    quint64 m_connectGeneration = 0;

    // Filled in from P2Client::discoveryInfoReceived() -- see capabilities()'s
    // own comment. Reset at the start of connectRadio() so a fresh connect
    // (possibly to a DIFFERENT host) never reports a prior radio's identity
    // before its own reply lands.
    bool m_discoveryInfoReceived = false;
    quint8 m_discoveredBoardId = 0;
    quint8 m_discoveredFirmwareVer = 0;
    quint8 m_discoveredNumDdc = 0;

    // scheduleTuneApply()'s leading+trailing throttle state. Lives on this
    // object's own thread (the GUI thread), not m_ioThread.
    static constexpr int kTuneThrottleMs = 33;   // ~30 Hz ceiling on real DDC retunes
    QTimer* m_tuneThrottleTimer = nullptr;
    bool m_tunePendingApply = false;

    // setPanBandwidth() serialization: one rate change at a time, gated on
    // m_rateChanging clearing (linkUp, or a finishDspSetup() failure), not a fixed
    // cooldown -- cold FFTW planning can take seconds, and overlapping restarts trip
    // P2Client's 2 s connect watchdog. At most one request is remembered while busy;
    // a newer one supersedes it.
    int m_pendingBandwidthKsps = 0;   // 0 = none pending
    int m_pendingBandwidthReceiver = 0;   // whose pan asked for it
    // Set from beginRateChange() through the next linkUp (or a failure path). Also
    // suppresses connected()/disconnected() churn: a zoom is not a disconnect.
    bool m_rateChanging = false;
    // Audio mute after a rate-change linkUp so a fresh WdspChannel settles before
    // live RF (see the linkUp handler). Display is unaffected by the mute.
    static constexpr int kRateChangeAudioSettleMs = 300;
    // Connect timeout for the rate-change restart path only: the just-stopped radio
    // needs settle time to re-arm its DDC pipeline, and the 2 s first-connect
    // default fires spurious connection errors on it.
    static constexpr int kRateChangeConnectTimeoutMs = 6000;
    // Settle window after a LIVE rate change: the new WdspChannel is installed
    // before the radio switches, so it briefly sees old-rate samples until p2app's
    // register write lands; audio stays muted across it (finishRateChange()). The
    // session never stops, so no restart settle is needed. 250 ms is a starting
    // value, bench-clean at 48 and 1536 ksps. It also replaces
    // kRateChangeAudioSettleMs here, but measured from the swap, so WDSP gets 250 ms
    // minus the handoff latency; if a rate change thumps on unmute, split the two
    // settles before raising this (#5547).
    static constexpr int kRateChangeLiveSettleMs = 250;

    // DDC-Specific resend offsets (ms) inside the settle window, on top of the
    // immediate send: a lost datagram costs nothing audible (see finishRateChange()).
    static constexpr int kRateChangeResendMs[] = {60, 140};

    // The CW pitch, one setting for every receiver.
    int m_cwPitchHz = 600;
    // What applyRestoredState() accepted for RX1, held for connectRadio(); kept
    // apart from its live freqHz/mode. Zero/empty: nothing restored.
    double m_restoredFreqHz = 0.0;
    QString m_restoredMode;

    // ---- speaker stream (DDC Audio, PC -> radio) ----
    //
    // RX1's audio only: a second receiver's mix into the radio's speaker is
    // its own change.
    //
    // Live copy of the connect-time parameter, so the audio path tests one bool
    // rather than reaching into m_pendingParams on every block.
    bool m_speakerAudioEnabled = false;
    // The radio's own output level/mute. 50 matches RadioModel's default, which it
    // resets to on every radio change; the two halves must agree. Also the
    // level applyRestoredState() returns to for a radio with none stored.
    static constexpr int kDefaultLineoutGainPercent = 50;
    int m_lineoutGainPercent = kDefaultLineoutGainPercent;
    bool m_lineoutMuted = false;
    // One resampler PER CHANNEL, never processStereoToStereo(), which averages to
    // mono and would undo the balance (as the engine's own output resampler,
    // docs/architecture/audio-pipeline.md).
    std::unique_ptr<Resampler> m_speakerResampleL;
    std::unique_ptr<Resampler> m_speakerResampleR;
    // Deinterleave/convert scratch, retained so a steady stream does not
    // allocate once per block in the audio path.
    std::vector<float> m_speakerSrcL;
    std::vector<float> m_speakerSrcR;
    QByteArray m_speakerOutL;
    QByteArray m_speakerOutR;
    std::vector<qint16> m_speakerInterleaved;

    // Per-radio settings identity ("anan" family): RadioConnectRequest::serial
    // (AnanDiscovery::macToSerial()), set at the top of connectRadio(); empty before
    // the first connect. isValid() needs only the family, so an empty serial would
    // silently target the family-wide row -- droopcal's `start` guards on an empty
    // radioId explicitly.
    QString m_radioSerial;
    AnanDroopCalibrator m_droopCalibrator;
    QString m_droopMessage;
    int m_droopPercent = 0;
    void publishDroopStatus();
    QString applyDroopTables(const QMap<int, DroopCorrectionTable>& tables);


    // RX1's rate actually running, captured at the top of
    // beginRateChange() before the pending fields are overwritten, so
    // finishRateChange()'s failure path can put them back. 0 until the first
    // rate change. See beginRateChange()'s own comment.
    int m_preRateChangeKsps = 0;
};

}  // namespace AetherSDR::anan
