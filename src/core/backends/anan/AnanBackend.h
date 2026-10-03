#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/AnanSliceAudio.h"
#include "core/Resampler.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/P2Client.h"
#include "core/dsp/WdspSMeter.h"

#include <QMap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR::anan {

// IRadioBackend for the ANAN-G2 (openHPSDR Protocol 2). Owns one P2Client and
// one AnanRxDsp on a dedicated I/O thread, like Hl2Backend with MetisClient +
// Hl2RxDsp. Scope: one DDC, RX only.
//   - No NCO/WDSP-shift split: DDC0 retunes on every setSliceFrequency()/
//     setPanCenter(), so "the shift is ALWAYS exactly -cwBfoHz(mode), because
//     the NCO IS the slice frequency".
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
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setSliceNoiseBlanker(int sliceId, bool on, int level) override;

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

    // The CW BFO offset (HERMES.md §5: "CW has no BFO unless you build one").
    // +pitchHz for CWU/CW, -pitchHz for CWL, 0 for every other mode -- zero
    // for non-CW is why every mode routes through this rather than only the
    // CW ones (entering/leaving CW must re-push the shift either way).
    [[nodiscard]] static double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept;

    // Snaps an operator-requested span to the nearest rate this radio
    // actually offers (capabilities().sampleRatesHz), by RATIO (log
    // distance), not linear -- see the definition comment for why linear
    // distance is provably wrong for these octave-spaced rates
    // (HERMES.md §15.1; mirrors Hl2Backend::nearestIqSampleRateHz()).
    // There is no continuous zoom here -- DDC0 runs at exactly one of six
    // fixed rates.
    [[nodiscard]] static int nearestDdc0RateKsps(int requestedKsps) noexcept;

    // Live operator AGC state as setSliceAgc() last stored it -- see the
    // member declaration comment for why beginRateChange() needs this
    // rather than reading connectRadio()'s connect-time snapshot. Exposed
    // read-only for testing without a live radio (matches WdspChannel's
    // own *ForTest accessor convention), not part of the operator-facing
    // seam.
    [[nodiscard]] int agcModeForTest() const noexcept { return m_agcMode; }
    [[nodiscard]] double agcCeilingDbForTest() const noexcept { return m_agcCeilingDb; }
    [[nodiscard]] int attenuationDbForTest() const noexcept { return m_attenuationDb; }
    [[nodiscard]] bool noiseBlankerOnForTest() const noexcept { return m_nbOn; }
    [[nodiscard]] int noiseBlankerLevelForTest() const noexcept { return m_nbLevel; }
    // The radio's own output level/mute as the lineout seam last set them. Same
    // *ForTest convention as the four above: read-only, not part of the seam.
    [[nodiscard]] int lineoutGainPercentForTest() const noexcept { return m_lineoutGainPercent; }
    [[nodiscard]] bool lineoutMutedForTest() const noexcept { return m_lineoutMuted; }
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
    void beginDspSetup();
    void finishDspSetup(quint64 generation, bool ok, const QString& error);
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
    AnanRxDsp::Config m_pendingDspConfig;
    // Panadapter points from the panel width (setPanPixelWidth()). Kept here
    // as well as in AnanRxDsp so a connect that happens after the width
    // arrives still builds the analyzer at that count.
    int m_panPoints = static_cast<int>(kDroopCorrectionFftSize);
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
    // Declares SLC:LEVEL to the meter seam; on every connect, before the
    // first reading can arrive. See its definition.
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
    void onDspMeter(float dbfs);
    // The same smoother Hl2Backend publishes through, so the two receivers'
    // needles move alike by construction -- see WdspSMeter.h.
    SMeterSmoother m_sMeter;
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
    void beginRateChange(int newRateKsps);
    // Runs once the background build (started by beginRateChange()) has
    // finished and been installed. Same body as the old beginRateChange()'s
    // second half, just triggered after the rebuild instead of before it --
    // see its own definition comment for the retry-window fix folded in
    // here too.
    void finishRateChange(quint64 generation, bool ok, const QString& error);

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
    [[nodiscard]] double cwBfoHz() const noexcept { return cwBfoOffsetHz(m_mode, m_cwPitchHz); }
    // Re-push mode + filter (with the CW BFO folded in) + shift, in that
    // order, to m_dsp. HERMES.md §16.7: mode changes must re-push the
    // passband, every time, not only when its value changed.
    void pushModeFilterShift();

    QThread* m_ioThread = nullptr;
    P2Client* m_client = nullptr;    // lives on m_ioThread; nullptr parent (moveToThread requires it)
    AnanRxDsp* m_dsp = nullptr;      // lives on m_ioThread; nullptr parent
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
    static constexpr int kTuneThrottleMs = 33;   // ~30 Hz ceiling on real DDC0 retunes
    QTimer* m_tuneThrottleTimer = nullptr;
    // Active ADC attenuation, mirrored in m_pendingParams for session restarts.
    // Captured/restored only through RadioStateMemory's rfGain extension.
    int m_attenuationDb = 0;
    bool m_tunePendingApply = false;

    // setPanBandwidth() serialization: one rate change at a time, gated on
    // m_rateChanging clearing (linkUp, or a finishDspSetup() failure), not a fixed
    // cooldown -- cold FFTW planning can take seconds, and overlapping restarts trip
    // P2Client's 2 s connect watchdog. At most one request is remembered while busy;
    // a newer one supersedes it.
    int m_pendingBandwidthKsps = 0;   // 0 = none pending
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

    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 100;
    int m_filterHighHz = 2900;
    int m_cwPitchHz = 600;
    double m_sliceFreqHz = 0.0;
    // Live AGC state, so beginRateChange() rebuilds the DSP config from CURRENT
    // state rather than connect-time defaults (which these match).
    int m_agcMode = 3;
    double m_agcCeilingDb = 60.0;
    // Noise blanker as setSliceNoiseBlanker() last stored it. Both
    // connectRadio() and beginRateChange() build the DSP config from it. This
    // differs from the AGC pair, which only beginRateChange() reads:
    // connectRadio() re-defaults AGC, but carries the blanker across a
    // reconnect. emitSliceState() also publishes the pair when a different
    // radio gets a fresh slice, keeping its NB button in agreement with the
    // retained setting. Defaults match AnanRxDsp::Config's.
    bool m_nbOn = false;
    int m_nbLevel = 50;

    // The receiver audio stage as last set; emitSliceState() publishes these.
    // Retained across rate changes and reconnects (like the blanker, unlike AGC).
    // 100/50 = unity, centred.
    bool m_sliceAudioMuted = false;
    int m_sliceAudioGainPercent = 100;
    int m_sliceAudioPanPercent = 50;

    // ---- speaker stream (DDC Audio, PC -> radio) ----
    //
    // Live copy of the connect-time parameter, so the audio path tests one bool
    // rather than reaching into m_pendingParams on every block.
    bool m_speakerAudioEnabled = false;
    // The radio's own output level/mute. 50 matches RadioModel's default, which it
    // resets to on every radio change; the two halves must agree.
    int m_lineoutGainPercent = 50;
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

    // Fixed identifiers -- Phase 1b is exactly one slice, one pan.
    static constexpr int kSliceId = 0;
    static const QString kPanId;

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


    // The DDC0 rate actually running, captured at the top of
    // beginRateChange() before the pending fields are overwritten, so
    // finishRateChange()'s failure path can put them back. 0 until the first
    // rate change. See beginRateChange()'s own comment.
    int m_preRateChangeKsps = 0;
};

}  // namespace AetherSDR::anan
