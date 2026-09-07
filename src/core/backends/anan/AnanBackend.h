#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/P2Client.h"

#include <QString>
#include <QThread>
#include <QTimer>

#include <utility>

namespace AetherSDR::anan {

// IRadioBackend implementor for the ANAN-G2 (openHPSDR Protocol 2). Owns one
// P2Client (the UDP session) and one AnanRxDsp (the WDSP demod + spectrum
// chain) on a dedicated I/O thread, mirroring how Hl2Backend owns MetisClient
// + Hl2RxDsp. Below the seam; RadioModel sees only this class.
//
// SCOPED TO aetherd ANAN P2 Phase 1b (02-working-plan.md Step 2): one DDC,
// RX only. Several things Hl2Backend's current (evolved) implementation does
// are deliberately absent here, not forgotten — see the design plan this was
// built from for the reasoning:
//   - No "keep the DDC fixed, move a WDSP shift" optimization. This backend
//     retunes DDC0 directly on every setSliceFrequency()/setPanCenter() call.
//     A direct consequence: the shift this class pushes to AnanRxDsp is
//     ALWAYS exactly -cwBfoHz(mode) -- there is no NCO-vs-slice offset term,
//     because the NCO IS the slice frequency, always.
//   - No live discovery read in connectRadio() -- capabilities() reports
//     hardcoded identity strings, matching Hl2Backend's own capabilities()
//     (its model string is hardcoded too).
//   - setKeying() is a guarded no-op. canTransmit is false and P2Client has
//     no PTT capability to call even if this method wanted to -- TX is a
//     separate, later addition (RFC §2.11 Phase 3), not a flag flip here.
//
// Receive handedness correctness is NOT this class's concern -- it lives
// entirely in AnanRxDsp's own conjugate split, unaffected by anything here.
// As of 2026-08-21 it's CONFIRMED, not just structurally borrowed from the
// HL2: `radiocert rx` plus an independent RSP1B/SDR++ receiver both agree.
// See AnanRxDsp.h and HERMES.md §16 for the full account.
class AnanBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit AnanBackend(QObject* parent = nullptr);
    ~AnanBackend() override;

    RadioCapabilities capabilities() const override;
    bool ownsRxAudio() const override { return true; }

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    void setCwPitch(int hz) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

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

private:
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
    void emitSliceState();
    void emitPanState();
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
    bool m_tunePendingApply = false;

    // setPanBandwidth() serialization: only one rate-change reconfigure runs
    // at a time, gated on m_rateChanging actually clearing (linkUp success,
    // or either failure path in finishDspSetup()) -- NOT a fixed-interval
    // throttle. A rate change is a full P2Client stop/reconfigure/restart
    // (beginRateChange()'s comment has the detail), and its duration is not
    // bounded: cold FFTW planning for a rate never used before in this
    // process can take seconds. An earlier fixed 250ms cooldown let a fast
    // zoom sweep queue up a second reconfigure before the first had
    // actually finished -- overlapping stop()/start() pairs on the same I/O
    // thread compounded into a delay long enough to trip P2Client's own
    // 2-second connect watchdog (measured on the bench). At most one
    // request is remembered while busy; a newer one supersedes an older.
    int m_pendingBandwidthKsps = 0;   // 0 = none pending
    // Set for the duration of a live rate-change reconfigure (beginRateChange()
    // through the next linkUp, or either finishDspSetup() failure path).
    // Doubles as setPanBandwidth()'s busy gate (above) and as the
    // connected()/disconnected() churn suppressor: from the operator's
    // perspective a zoom is not a disconnect, even though it is implemented
    // as one under the hood.
    bool m_rateChanging = false;
    // How long to hold audio muted after a rate-change linkUp before
    // unmuting -- see the linkUp handler's own comment for why a fresh
    // WdspChannel needs this. Spectrum/waterfall are untouched by the mute
    // (AnanRxDsp::setAudioMuted() only zeroes the audio-path input, per its
    // own header comment), so this does not affect how quickly the display
    // recovers, only when sound resumes.
    static constexpr int kRateChangeAudioSettleMs = 300;
    // P2Client::kConnectTimeoutMs (2000ms) is tuned for a first-ever connect.
    // A rate-change restart is different: the radio was just told to stop,
    // possibly after sitting idle for the seconds beginDspSetup()'s
    // configure() held the shared I/O thread (see beginRateChange()'s own
    // comment), and needs real settle time to re-arm its DDC pipeline before
    // streaming again. Bench testing showed the SHORT default firing a
    // "connection error" during a rate change that was already about to
    // recover on its own (P2Client keeps listening past its own timeout --
    // the message was spurious, not a real failure). Passed to
    // P2Client::start() only on the rate-change path; a genuine first
    // connect keeps the shorter, tighter default.
    static constexpr int kRateChangeConnectTimeoutMs = 6000;
    // Minimum idle time between stop() and start() on a rate-change restart.
    // Bench-discovered (2026-08-19), not a spec citation: with the DSP
    // rebuild moved off the stop/start path (background-rebuild fix), the
    // radio started NOT responding to a restart fired only ~100ms after
    // stop() -- "no DDC0 IQ within 6000ms", followed by a full disconnect/
    // reconnect to recover. The OLD synchronous-rebuild architecture never
    // hit this: the slow rebuild sat BETWEEN stop() and start(), so the
    // radio always got real idle time before a restart, as an unintended
    // side effect of what was otherwise a bug. This constant restores that
    // gap on purpose instead of by accident. Value is a bench guess, not
    // yet a confirmed minimum -- see finishRateChange()'s own comment.
    // 500ms was tried first (2026-08-19) and was NOT reliably enough: a
    // chained rapid zoom sequence still hit the 6s timeout and forced a
    // full reconnect. Raised to 2000ms to test next.
    static constexpr int kRateChangeRestartSettleMs = 2000;

    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 100;
    int m_filterHighHz = 2900;
    int m_cwPitchHz = 600;
    double m_sliceFreqHz = 0.0;
    // Live operator AGC state -- setSliceAgc() had no backing member before
    // this; needed so beginRateChange() can refresh m_pendingDspConfig from
    // CURRENT state instead of connectRadio()'s connect-time snapshot (a
    // rate change used to silently revert AGC to whatever it was at connect,
    // same bug class as the mode/filter staleness this fixes alongside it).
    // Defaults match connectRadio()'s own connect-time defaults.
    int m_agcMode = 3;
    double m_agcCeilingDb = 60.0;

    // Fixed identifiers -- Phase 1b is exactly one slice, one pan.
    static constexpr int kSliceId = 0;
    static const QString kPanId;
};

}  // namespace AetherSDR::anan
