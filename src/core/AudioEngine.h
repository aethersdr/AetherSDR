#pragma once

#include <QObject>
#include <QAudioSink>
#include <QAudioSource>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QUdpSocket>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <QBuffer>
#include <QByteArray>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFutureSynchronizer>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "CwRecordGate.h"
#include "TxMicChannelNormalizer.h"
#include "TxCaptureHealthTracker.h"
#include "SpectralNR.h"
#include "OpusTxPacer.h"

class QMediaDevices;

#include <functional>
#include <algorithm>
#include <memory>
#include <deque>
#include <vector>
#include <cstdint>

namespace AetherSDR {

class SpecbleachFilter;
class RNNoiseFilter;
class DeepFilterFilter;
class NvidiaAfxFilter;
class Resampler;
class TxVoiceProcessor;
class ClientEq;
class ClientComp;
class ClientGate;
class ClientDeEss;
class ClientTube;
class ClientPudu;
class ClientPuduMonitor;
class ClientReverb;
class ClientFinalLimiter;
class ClientTxTestTone;
class ClientQuindarTone;
class WsprBeacon;
class QuindarLocalSink;
class CwSidetoneGenerator;
#ifdef __APPLE__
class MacNRFilter;
#endif

// AudioEngine handles audio playback (RX) and capture (TX).
//
// RX path:
//   Audio PCM arrives via PanadapterStream::audioDataReady() — the radio sends
//   VITA-49 IF-Data packets to the single "client udpport" socket owned by
//   PanadapterStream. PanadapterStream strips the header and emits the raw PCM;
//   connect that signal to feedAudioData() then call startRxStream() to open
//   the QAudioSink.
//
// TX path:
//   Captures mic/input audio via QAudioSource, frames it as VITA-49
//   ExtDataWithStream packets (PCC 0x03E3, float32 stereo big-endian),
//   and sends to the radio via UDP.

class AudioEngine : public QObject {
    Q_OBJECT

public:
    static constexpr int DEFAULT_SAMPLE_RATE = 24000;

    struct ReceivePresentationAudioQueues {
        int playbackQueuedMs{0};
        int flexRawBufferMs{0};
        int flexOutputBufferMs{0};
        int kiwiSdrRawBufferMs{0};
        int kiwiSdrOutputBufferMs{0};
        int externalKiwiRawBufferMs{0};
        int externalKiwiOutputBufferMs{0};

        int flexTotalQueuedMs() const
        {
            return flexRawBufferMs + flexOutputBufferMs + playbackQueuedMs;
        }

        int kiwiTotalQueuedMs() const
        {
            return std::max(kiwiSdrRawBufferMs + kiwiSdrOutputBufferMs,
                            externalKiwiRawBufferMs + externalKiwiOutputBufferMs)
                   + playbackQueuedMs;
        }
    };

    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    // Open the QAudioSink. Must be called once when connected.
    // Q_INVOKABLE: must run on the audio worker thread (#502)
    Q_INVOKABLE bool startRxStream();
    Q_INVOKABLE void stopRxStream();

    // Dedicated low-latency sidetone sink — independent of the RX sink.
    // Started alongside the RX sink and on-demand when the user enables
    // local sidetone for the first time after connect.
    Q_INVOKABLE bool startSidetoneStream();
    Q_INVOKABLE void stopSidetoneStream();
    Q_INVOKABLE bool startQuindarLocalSink();
    Q_INVOKABLE void stopQuindarLocalSink();

    // TX (microphone) – capture audio and send VITA-49 packets to radio
    Q_INVOKABLE bool startTxStream(const QHostAddress& radioAddress, quint16 radioPort);

    // Host-modulating backend (HL2): run the TX audio chain even though no Flex
    // stream id will ever be assigned.
    //
    // onTxAudioReady() gates on a stream id because for Flex that id IS the
    // destination — no id means nowhere to send. A backend that modulates
    // locally has a destination regardless, and the gate silently disabled the
    // test tone as well, since the tone is injected inside that callback.
    Q_INVOKABLE void setHostModulation(bool on) { m_hostModulation = on; }
    bool hostModulation() const { return m_hostModulation; }
    Q_INVOKABLE void stopTxStream();

    // Set the DAX TX stream ID (from radio's response to "stream create type=dax_tx")
    void setTxStreamId(quint32 id) { m_txStreamId = id; }
    quint32 txStreamId() const { return m_txStreamId; }
    // Set the remote audio TX stream ID (for voice TX and VOX monitoring)
    void setRemoteTxStreamId(quint32 id) { m_remoteTxStreamId = id; }
    quint32 remoteTxStreamId() const { return m_remoteTxStreamId; }
    // True when either TX stream (dax_tx or remote_audio_tx) is assigned —
    // mic capture should run if any TX route to the radio exists.
    bool hasAnyTxStream() const { return m_txStreamId != 0 || m_remoteTxStreamId != 0; }

    float rxVolume() const  { return m_rxVolume.load(); }
    void  setRxVolume(float v);
    void  setRxBoost(bool on) { m_rxBoost.store(on); }
    bool  rxBoost() const { return m_rxBoost.load(); }
    // Final post-DSP linear gain on the RX path (-12..+12 dB).  Applied
    // after the optional soft-knee BOOST so the strip's RX output meter
    // (which taps post-trim) reads what hits the speakers.  Mirrors
    // ClientFinalLimiter::outputTrimDb on the TX side.
    void  setRxOutputTrimDb(float db) { m_rxOutputTrimDb.store(db); }
    float rxOutputTrimDb() const { return m_rxOutputTrimDb.load(); }

    // Client-side RX pan (0=full-left, 50=centre, 100=full-right).
    // Normally the radio handles Flex panning. External single-source audio
    // still uses this as an output pan after stereo-preserving client DSP.
    void setRxPan(int panValue);
    int  rxPan() const { return m_rxPan.load(); }
    void  setRxBufferCapMs(int ms) { m_rxBufferCapMs.store(qBound(50, ms, 1000)); }
    int   rxBufferCapMs() const { return m_rxBufferCapMs.load(); }
    void setReceivePresentationDelays(
        int flexDelayMs,
        int kiwiDelayMs,
        const QString& externalKiwiDelaySourceId = QString());
    void resetReceivePresentationAudioBuffers();
    void resetReceivePresentationAudioBuffersForKiwiSource(
        const QString& sourceId);

    bool isMuted() const       { return m_muted.load(); }
    void setMuted(bool m);
    bool isRxStreaming() const { return m_audioSink != nullptr; }
    bool isTxStreaming() const { return m_audioSource != nullptr; }
    // Which producer currently owns the QSO recorder's TX slot (#2539,
    // #4281). Both the mic monitor tap and the CW record pump reach
    // QsoRecorder::feedTxAudio, so both ends ask this one question rather
    // than each keeping its own idea of who is recording. See
    // CwRecordGate.h for why mic-capture state is not an input.
    TxRecorderSource txRecorderSource() const {
        // The first input stays the RAW any-owner interlock, deliberately not
        // cwOverTxActive: the Mic-vs-None distinction only matters while the
        // recorder's MOX-gated TX gate is open, which a foreign transmission
        // never opens — so narrowing it here would change nothing and would
        // break the two-input contract the test pins (#4281).
        return AetherSDR::txRecorderSource(
            m_radioTransmitting.load(std::memory_order_acquire),
            // "our CW over is in progress" = our keyer fired AND the radio has
            // keyed during it. Not the raw latch: see m_cwOverHadTx.
            m_cwKeyedThisOver.load(std::memory_order_acquire)
                && m_cwOverHadTx.load(std::memory_order_acquire));
    }
    // Mirror of TransmitModel::cwSpeed, used only to size the CW over-hang.
    void setCwWpm(int wpm) {
        if (wpm > 0) {
            m_cwWpm.store(wpm, std::memory_order_relaxed);
        }
    }
    // Whether a Client-Side recording is open, so the CW record pump can skip
    // rendering samples nothing will store (#4281). Orthogonal to ownership:
    // that answers WHOSE audio belongs in the file, this answers whether there
    // is a file. Stored from QsoRecorder's start/stop signals.
    void setQsoRecordingActive(bool on) {
        m_qsoRecordingActive.store(on, std::memory_order_release);
    }
    // Mirror of "the TX slice's mode is a CW mode" (VoiceModeGate isCwMode),
    // pushed by MainWindow::updateKeyerAvailability() on every mode change and
    // TX-slice reassignment. Gates the over latch: RadioModel::sendCwKey has
    // no mode gate (TCI/MIDI/HID/serial key edges arrive in any mode), so
    // without this a brushed paddle during an SSB over latches the CW over
    // machinery and hands the rest of the voice over to the record pump
    // (#4281). Leaving a CW mode ends any over in flight: both latches clear,
    // and the pump's next tick sees ownership drop and closes the recorder's
    // CW gate — so a voice over begun within the old hang records the mic.
    void setTxModeCw(bool cw) {
        const bool was = m_txModeIsCw.exchange(cw, std::memory_order_acq_rel);
        if (was && !cw) {
            m_cwKeyedThisOver.store(false, std::memory_order_release);
            m_cwOverHadTx.store(false, std::memory_order_release);
            m_cwOverWpm.store(0, std::memory_order_relaxed);
        }
    }
    // Mirror of TransmitModel's tune state (optimistic-local, then status-
    // reconciled). A tune carrier raises the interlock as an owned TX but is
    // not a CW over; cwOverTxActive excludes it (#4281).
    void setTuneActive(bool tuning) {
        m_tuneActive.store(tuning, std::memory_order_release);
    }
    // A speed our keyer will send THIS over at, min-tracked into the over-hang
    // (#4281): CWX keys at CwxModel's own per-segment wpm, independent of the
    // TransmitModel::cwSpeed mirror, and a 15 WPM segment against a 30 WPM
    // mirror would age the latch inside every 560 ms word gap (hang: 320 ms).
    // All of a message's segments announce at send time, so min-tracking
    // sizes the hang from the message's slowest speed; see
    // cwOverHangMs(int, int).
    void noteCwOverSpeed(int wpm) {
        if (wpm <= 0) {
            return;
        }
        int cur = m_cwOverWpm.load(std::memory_order_relaxed);
        while ((cur == 0 || wpm < cur)
               && !m_cwOverWpm.compare_exchange_weak(
                      cur, wpm, std::memory_order_relaxed)) {
        }
    }
    bool kiwiSdrAudioTransmitMuted() const;
    bool hasKiwiSdrAudioSource(const QString& sourceId) const;
    int  txInputSampleRate() const { return m_txInputRate; }
    int  txInputChannelCount() const { return m_txInputChannels; }
    QAudioFormat::SampleFormat txInputSampleFormat() const { return m_txInputFormat; }
    // True when the mic hands us the host engine's float mix directly, so the
    // 48 kHz float voice strip is fed without an Int16 round trip on ingress.
    bool txInputIsFloat32() const { return m_txInputFormat == QAudioFormat::Float; }
    int  txInputBytesPerSample() const { return txInputIsFloat32() ? 4 : 2; }
    bool txInputNormalizationTo48k() const;
    bool txRadeResamplingTo24k() const { return m_radeTxNeedsResample; }
    bool rxOutputResamplingActive() const { return m_rxOutputRate.load() != DEFAULT_SAMPLE_RATE; }
    QJsonArray audioEndpointDiagnostics() const;
    QJsonObject startAutomationAudioCapture(int durationMs,
                                            const QStringList& points);
    QJsonObject stopAutomationAudioCapture();
    QJsonObject automationAudioCaptureSnapshot(bool includePcm) const;
    QJsonObject automationNr2StereoProbe() const;
    QJsonObject automationDspStereoProbe(const QString& mode) const;

    // Client-side PC mic gain (0-100 → 0.0-1.0, applied before Opus encoding)
    void setPcMicGain(int level) { m_pcMicGain.store(qBound(0, level, 100) / 100.0f); }

    // Opus TX encoding for SmartLink compressed audio
    void setOpusTxEnabled(bool on) { m_opusTxEnabled.store(on); }
    bool isOpusTxEnabled() const { return m_opusTxEnabled.load(); }

    // RADE digital voice mode
    void setRadeMode(bool on);
    bool isRadeMode() const { return m_radeMode; }

    // Sends RADE modem output (float32 PCM) as VITA-49 packets via m_txSocket
    void sendModemTxAudio(const QByteArray& float32pcm);

    // DAX TX: VirtualAudioBridge feeds float32 PCM for VITA-49 TX
    void setDaxTxMode(bool on);
    bool isDaxTxMode() const { return m_daxTxMode.load(); }
    // true: radio DAX TX route (transmit dax=1, PCC 0x0123 int16 mono)
    // false: low-latency PC mic route (transmit dax=0, PCC 0x03E3 float32 stereo)
    void setDaxTxUseRadioRoute(bool on);
    bool daxTxUseRadioRoute() const { return m_daxTxUseRadioRoute.load(); }
    void setTransmitting(bool tx);
    // Raw interlock state plus its tx_client_handle attribution, delivered as
    // one snapshot (the parse stores ownership before emitting the signal).
    void setRadioTransmitting(bool tx, bool ownedByUs);
    // Self-marshals onto the AudioEngine thread; safe from any caller.
    Q_INVOKABLE void clearTxAccumulators();
    Q_INVOKABLE void feedDaxTxAudio(const QByteArray& float32pcm);

    // Plays RADE decoded speech (int16 stereo 24kHz) bypassing m_radeMode block
    void feedDecodedSpeech(const QByteArray& pcm);

    // Client-side NR2 (spectral noise reduction)
    // Q_INVOKABLE: called from main thread, runs on audio worker thread (#502)
    Q_INVOKABLE void setNr2Enabled(bool on);
    bool nr2Enabled() const { return m_nr2Enabled.load(); }
    // NR2 user-adjustable parameters (thread-safe via atomic in SpectralNR)
    void setNr2GainMax(float v);
    void setNr2GainFloor(float v);
    void setNr2Qspp(float v);
    void setNr2GainSmooth(float v);
    void setNr2GainMethod(int method);
    void setNr2NpeMethod(int method);
    void setNr2AeFilter(bool on);
    QJsonObject nr2RuntimeDiagnostics() const;
    QJsonObject opusTxPacingDiagnostics() const;
    // Tell the engine the main RX source is (or is not) the demo, so the main NR2
    // filter uses the original 256/2 geometry the demo's tiny frames need. Rebuilds
    // the active main NR2 filter if enabled so the change takes effect immediately.
    Q_INVOKABLE void setMainSourceLegacyNr2(bool legacy);
    // Client-side RN2 (RNNoise neural noise suppression)
    Q_INVOKABLE void setRn2Enabled(bool on);
    bool rn2Enabled() const { return m_rn2Enabled.load(); }
    // RN2 dry mix — fraction of the original spectrum RN2 leaves in the RX
    // output (Rn2SettingsModel owns the value). Applies to every live RX RN2
    // instance; the TX path keeps full suppression.
    void setRn2DryMix(float value);

    // Client-side RN2 — TX path (mic pre-amp).  Runs on the voice path
    // in onTxAudioReady() AFTER the RADE/DAX early-returns, so digital
    // modes never see RN2.  Separate instance + atomic from m_rn2 so
    // RX and TX can be toggled independently.  (#2813)
    Q_INVOKABLE void setRn2TxEnabled(bool on);
    bool rn2TxEnabled() const { return m_rn2TxEnabled.load(); }

    // Client-side NR4 (libspecbleach spectral noise reduction)
    Q_INVOKABLE void setNr4Enabled(bool on);
    bool nr4Enabled() const { return m_nr4Enabled.load(); }
    void setNr4ReductionAmount(float dB);
    void setNr4SmoothingFactor(float pct);
    void setNr4WhiteningFactor(float pct);
    void setNr4AdaptiveNoise(bool on);
    void setNr4NoiseEstimationMethod(int method);
    void setNr4MaskingDepth(float v);
    void setNr4SuppressionStrength(float v);

    // Client-side MNR (macOS MMSE-Wiener spectral noise reduction)
    Q_INVOKABLE void setMnrEnabled(bool on);
    bool mnrEnabled() const { return m_mnrEnabled.load(); }
    void setMnrStrength(float normalized);
    float mnrStrength() const;

    // Client-side DFNR (DeepFilterNet3 neural noise reduction)
    Q_INVOKABLE void setDfnrEnabled(bool on);
    bool dfnrEnabled() const { return m_dfnrEnabled.load(); }
    void setDfnrAttenLimit(float db);
    float dfnrAttenLimit() const;
    void setDfnrPostFilterBeta(float beta);

    // Optional NVIDIA Maxine AFX GPU denoiser (runtime-loaded; NVIDIA RTX/GeForce).
    Q_INVOKABLE void setNvAfxEnabled(bool on);
    bool nvAfxEnabled() const { return m_nvAfxEnabled.load(); }
    void setNvAfxIntensity(float ratio);

    // Client-side parametric EQ. Two instances: one on the RX audio path
    // (post-NR, pre-write to sink), one on the TX path (post-mic, pre-
    // VITA-49 encode). Both are independent from the radio-side EQ.
    // Returns non-null pointers after first prepare()/startRxStream.
    ClientEq* clientEqRx() { return m_clientEqRx.get(); }
    ClientEq* clientEqTx() { return m_clientEqTx.get(); }

    // Client-side TX dynamics processor (Pro-XL-style compressor +
    // brickwall limiter, #1661).  Runs on the TX audio path only.
    // Execution order is controlled by the TX chain order below.
    ClientComp* clientCompTx() { return m_clientCompTx.get(); }
    ClientComp* clientCompRx() { return m_clientCompRx.get(); }

    // Client-side TX downward expander / noise gate (#1661 Phase 2).
    // Single DSP module with an Expander ↔ Gate mode toggle that
    // snaps ratio + floor to known-good presets.
    ClientGate* clientGateTx() { return m_clientGateTx.get(); }
    ClientGate* clientGateRx() { return m_clientGateRx.get(); }

    // Client-side TX de-esser (#1661 Phase 3).  Sidechain-filtered
    // dynamics: a user-tunable bandpass feeds the envelope detector,
    // broadband attenuation (capped at amountDb) is applied when
    // sibilant energy crosses threshold.
    ClientDeEss* clientDeEssTx() { return m_clientDeEssTx.get(); }
    ClientDeEss* clientDeEssRx() { return m_clientDeEssRx.get(); }

    // Client-side TX dynamic tube saturator (#1661 Phase 4).  Three
    // selectable curves, bipolar envelope-driven drive, tilt tone
    // pre-filter, parallel dry/wet mix.
    ClientTube* clientTubeTx() { return m_clientTubeTx.get(); }
    ClientTube* clientTubeRx() { return m_clientTubeRx.get(); }

    // Client-side TX exciter — PUDU (#1661 Phase 5).  Aphex-lineage
    // vs. Behringer-lineage two-band exciter, the centrepiece of the
    // PooDoo Audio™ chain.
    ClientPudu* clientPuduTx() { return m_clientPuduTx.get(); }
    ClientPudu* clientPuduRx() { return m_clientPuduRx.get(); }

    // Client-side TX reverb — Freeverb-based room/hall effect, final
    // optional stage in the PooDoo™ chain.
    ClientReverb* clientReverbTx() { return m_clientReverbTx.get(); }

    // Final brickwall limiter — sits at the very tail of the TX audio
    // path, after every user-configurable chain stage AND after PC mic
    // gain, so no sample escapes louder than the configured ceiling
    // regardless of upstream behaviour.  Always present (not in the
    // user-reorderable chain).  Enable / ceiling persisted via
    // AppSettings under ClientFinalLimiter*.
    ClientFinalLimiter* clientFinalLimiterTx() { return m_clientFinalLimiterTx.get(); }

    // Test-tone generator at the head of the TX path.  When enabled,
    // overrides mic input with a sine before the user's DSP chain
    // runs — useful for setup / calibration.
    ClientTxTestTone* clientTxTestTone() { return m_clientTxTestTone.get(); }

    // Sample-accurate WSPR source. It replaces the post-voice-chain signal
    // on its own paced DAX/VITA-49 path, so speech processing and microphone
    // callback rates cannot distort or shorten the four-tone frame.
    WsprBeacon* wsprBeacon() { return m_wsprBeacon.get(); }
    Q_INVOKABLE void startWsprPump();
    Q_INVOKABLE void stopWsprPump();

    // Quindar tone generator (#2262).  Sits AFTER the user DSP chain
    // and PC mic gain but BEFORE the final brickwall limiter, so the
    // generated tone is unprocessed by Comp/EQ but still bounded by
    // the configured ceiling.  Driven by the TransmitModel PTT
    // coordinator on phone modes; bypassed on CW / digital.
    ClientQuindarTone* clientQuindarTone() { return m_clientQuindarTone.get(); }

    // Register a monitor that taps the post-final-limiter int16
    // stream — the exact bytes that get packetised into VITA-49.
    // Mirror of setTxPostDspMonitor but at the chain tail.
    void setTxFinalMonitor(ClientPuduMonitor* m) noexcept;

    // Register/unregister a monitor that taps the post-DSP TX int16
    // stream on the audio thread.  Passed pointer must outlive the
    // registration — clear to nullptr before destroying the monitor.
    void setTxPostDspMonitor(ClientPuduMonitor* m) noexcept;

    // Generalised TX DSP chain — each stage is a separate float processing
    // block run in order by TxVoiceProcessor at 48 kHz. Numeric values are
    // persisted in the packed atomic and therefore form a stable contract.
    enum class TxChainStage : uint8_t {
        None   = 0,   // sentinel / end-of-list marker
        Gate   = 1,
        Eq     = 2,
        DeEss  = 3,
        Comp   = 4,
        Tube   = 5,
        Enh    = 6,   // PUDU slot
        Reverb = 7,
    };
    static constexpr int kMaxTxChainStages = 8;  // packs into uint64_t

    // RX chain — same packed-atomic dispatcher pattern as TX, applied
    // to playback audio after the radio's NR has run.  Phase 0 ships
    // the framework with no implemented stages; Phase 1+ slot in the
    // RX-side ClientEq, ClientGate, ClientComp, ClientTube, ClientPudu
    // instances.  See plans/poodoo-rx-chain.md.
    enum class RxChainStage : uint8_t {
        None  = 0,   // sentinel / end-of-list marker
        Eq    = 1,
        Gate  = 2,
        Comp  = 3,
        Tube  = 4,
        Pudu  = 5,
        DeEss = 6,
    };
    static constexpr int kMaxRxChainStages = 8;  // packs into uint64_t

    void setRxChainStages(const QVector<RxChainStage>& stages);
    QVector<RxChainStage> rxChainStages() const;
    void loadClientRxChainOrder();
    void saveClientRxChainOrder() const;

    // Set the ordered list of stages.  UI thread API; commits an
    // atomic snapshot that the audio thread picks up on its next block.
    // Order may contain any subset of stages; unlisted stages are
    // bypassed entirely.  Persists via AppSettings.
    void setTxChainStages(const QVector<TxChainStage>& stages);
    QVector<TxChainStage> txChainStages() const;

    // ── Master TX bypass ─────────────────────────────────────────
    // Single source of truth for the chain-wide BYPASS state shared
    // between the docked Chain applet's BYPASS button and the channel
    // strip's BYPASS button.  setTxBypassed(true) snapshots the
    // currently-enabled TX stages and disables them all; calling
    // setTxBypassed(false) restores only those stages that were on
    // before bypass engaged.  Stages flipped on manually while bypass
    // was active survive the restore (they're not in the snapshot).
    // Emits txBypassChanged(bool) on transitions.
    void setTxBypassed(bool on);
    bool isTxBypassed() const;

    // ── Master RX bypass ─────────────────────────────────────────
    // Sibling of the TX bypass plumbing above.  Same snapshot-and-
    // restore semantics applied to the RX chain.  Stages without an
    // implemented DSP class (currently DeEss) are skipped — they're
    // already no-ops in the dispatcher, so disabling them is moot.
    // Emits rxBypassChanged(bool) on transitions.
    void setRxBypassed(bool on);
    bool isRxBypassed() const;

    // Legacy two-stage API — the existing ClientCompEditor combo box
    // still drives this during the transition to the generalised chain.
    // Reads the relative position of Comp vs Eq in the current chain.
    // Set swaps those two stages in-place without disturbing the others.
    // To be removed when the CHAIN applet takes over chain editing.
    enum class TxChainOrder {
        CompThenEq = 0,
        EqThenComp = 1,
    };
    void setTxChainOrder(TxChainOrder order);
    TxChainOrder txChainOrder() const;

    void loadClientCompSettings();
    void loadClientCompRxSettings();
    void saveClientCompRxSettings() const;
    void saveClientCompSettings() const;

    // Client-side TX gate — persistence mirrors the compressor.
    void loadClientGateSettings();
    void loadClientGateRxSettings();
    void saveClientGateRxSettings() const;
    void saveClientGateSettings() const;

    // Client-side TX de-esser — persistence.
    void loadClientDeEssSettings();
    void saveClientDeEssSettings() const;
    // RX-side counterpart (#2425).
    void loadClientDeEssRxSettings();
    void saveClientDeEssRxSettings() const;

    // Client-side TX dynamic tube — persistence.
    void loadClientTubeSettings();
    void loadClientTubeRxSettings();
    void saveClientTubeRxSettings() const;
    void saveClientTubeSettings() const;

    // Client-side TX PUDU exciter — persistence.
    void loadClientPuduSettings();
    void loadClientPuduRxSettings();
    void saveClientPuduRxSettings() const;
    void saveClientPuduSettings() const;

    // Client-side TX reverb (Freeverb) — persistence.
    void loadClientReverbSettings();
    // Non-const because it emits clientReverbStateChanged() at the
    // end so any UI bound to the reverb (strip panel, docked applet,
    // floating editor) can refresh from a single notification path
    // instead of polling on a timer.
    void saveClientReverbSettings();
    void loadClientFinalLimiterSettings();
    void saveClientFinalLimiterSettings() const;
    // Aetherial Tube Pre-Amp TX state — nested-JSON shape under one key
    // so future mic-preamp toggles (high-pass, phase invert, etc.) can
    // land without further migration.  Today: {"rn2": bool}.  (#2813)
    void loadAetherialTubePreampTxSettings();
    void saveAetherialTubePreampTxSettings() const;
    void loadClientQuindarSettings();
    void saveClientQuindarSettings() const;

    // Post-Client-EQ audio tap for the editor's FFT analyzer.  Exposes
    // a rolling mono buffer filled on the audio thread; UI thread copies
    // the most-recent N samples for FFT without allocating or blocking.
    // `out` is filled newest-last (FIFO-style), returns true on success.
    static constexpr int kClientEqTapSize = 2048;  // ~85ms at 24 kHz
    bool copyRecentClientEqRxSamples(float* out, int count) const;
    bool copyRecentClientEqTxSamples(float* out, int count) const;

    // Load/save all EQ state (enable flag, active band count, per-band
    // params) via AppSettings. `path` is "Rx" or "Tx" — used as the key
    // prefix. Call loadClientEqSettings() once at startup (before the
    // applet wires up); saves happen live from the applet as the user
    // edits.
    void loadClientEqSettings();
    void saveClientEqSettings() const;

    // Ensure FFTW wisdom is loaded/generated. Returns true if wisdom
    // needs to be generated (slow). Call generateWisdom() in that case.
    static bool needsWisdomGeneration();
    static QString wisdomFilePath();
    // Must be called from a worker thread — blocks for several minutes.
    static SpectralNR::WisdomResult generateWisdom(
        SpectralNR::WisdomProgressCb progress = nullptr,
        SpectralNR::WisdomCancelCb shouldCancel = nullptr);

    // Device selection (restarts the stream if currently running)
    void setOutputDevice(const QAudioDevice& dev);
    void setInputDevice(const QAudioDevice& dev);
#ifdef Q_OS_MAC
    void setAllowBluetoothTelephonyOutput(bool on);
#endif
    QAudioDevice outputDevice() const { return m_outputDevice; }
    QAudioDevice inputDevice()  const { return m_inputDevice; }
    qsizetype rxBufferBytes() const { return m_rxBufferBytes.load(); }
    qsizetype rxBufferPeakBytes() const { return m_rxBufferPeakBytes.load(); }
    quint64 rxBufferUnderrunCount() const { return m_rxBufferUnderrunCount.load(); }
    int rxBufferSampleRate() const { return m_rxBufferSampleRate.load(); }
    int rxPlaybackQueuedMs() const
    {
        return m_rxPlaybackQueuedMs.load(std::memory_order_relaxed);
    }
    ReceivePresentationAudioQueues receivePresentationAudioQueues() const;
    quint64 receivePresentationOutputSignalEmitCount() const
    {
        return m_receivePresentationOutputSignalEmitCount.load(
            std::memory_order_relaxed);
    }
    quint64 receivePresentationOutputSignalSuppressedCount() const
    {
        return m_receivePresentationOutputSignalSuppressedCount.load(
            std::memory_order_relaxed);
    }

    // Local CW sidetone generator — accessor used by RadioModel signal
    // routing and PhoneCwApplet UI bindings.
    CwSidetoneGenerator* cwSidetone() { return m_cwSidetone.get(); }

    // Key BOTH the audible sidetone and the recorder-sidetone generator from one
    // call so every local CW source (manual keyer, CWX macros, iambic paddle)
    // drives them in lockstep. The recorder copy is what lets a Client-Side QSO
    // recording capture the operator's own sent CW/CWX side-tone (#2539).
    // `when` is the edge's scheduled instant on the producer's element grid
    // (#4890): keying sources with an exact schedule (iambic keyer) pass their
    // grid deadline so the sidetone renders intended rhythm, not thread-wake
    // rhythm; sources without one take the wall-clock default.
    // Note the deliberate asymmetry with RadioModel::sendCwKeyEdge, whose
    // `scheduledAt` uses a default-constructed (epoch) time_point as an
    // explicit "no schedule" sentinel it tests for.  Here the sidetone needs a
    // usable instant on every call, so "no schedule" is spelled now() and
    // there is nothing to test for — passing {} would stamp the epoch rather
    // than mean "unscheduled".
    void setCwKeyDown(bool down,
                      std::chrono::steady_clock::time_point when =
                          std::chrono::steady_clock::now());

    // Start the CW-sidetone record pump (#2539). CW has no mic-driven
    // onTxAudioReady, so a free-running timer on the audio thread feeds the
    // recorder the local sidetone while the radio is keyed for CW. Idempotent;
    // must be invoked on the audio thread (queued) after moveToThread().
    void startCwRecordPump();

    // Enable/disable the TX-side CW-decode tap (#2417).  When enabled,
    // the sidetone generator's mono signal is mirrored — downsampled
    // from 48 kHz to 24 kHz stereo float — and emitted via
    // txDecodeAudioReady() so MainWindow can hand it to CwDecoder.
    // The tap is also gated by sidetone enable; if the operator has
    // turned local sidetone off there is nothing to decode.
    void setCwDecodeTxTapEnabled(bool on)
    {
        m_cwDecodeTxTapEnabled.store(on, std::memory_order_relaxed);
    }
    bool isCwDecodeTxTapEnabled() const
    {
        return m_cwDecodeTxTapEnabled.load(std::memory_order_relaxed);
    }

    void setTncRxTapEnabled(bool on)
    {
        m_tncRxTapEnabled.store(on, std::memory_order_relaxed);
    }
    bool isTncRxTapEnabled() const
    {
        return m_tncRxTapEnabled.load(std::memory_order_relaxed);
    }

public slots:
    // Receives stripped PCM from PanadapterStream::audioDataReady().
    void feedAudioData(const QByteArray& pcm);
    // Receives decoded KiwiSDR PCM after a clean protocol decoder exists.
    // Same format as feedAudioData(): 24 kHz stereo float32.
    void feedKiwiSdrAudioData(const QByteArray& pcm24kStereoFloat);
    void feedKiwiSdrAudioData(const QString& sourceId,
                              const QByteArray& pcm24kStereoFloat);
    void setKiwiSdrAudioEnabled(bool on);
    void setKiwiSdrAudioSourceEnabled(const QString& sourceId, bool on);
    void setKiwiSdrAudioSourceGain(const QString& sourceId, float gainPercent);
    void setKiwiSdrAudioSourceMuted(const QString& sourceId, bool muted);
    void setKiwiSdrAudioSourceKeepDuringTx(const QString& sourceId, bool keep);
    void setKiwiSdrAudioSourceResumeHold(const QString& sourceId, int holdMs);
    void setKiwiSdrAudioSourcePan(const QString& sourceId, int pan);
    void setKiwiSdrAudioTransmitMuted(bool muted);
    void removeKiwiSdrAudioSource(const QString& sourceId);

signals:
    void rxStarted();
    void rxStopped();
    void levelChanged(float rms);  // audio level for VU meter, 0.0–1.0
    void nr2EnabledChanged(bool on);
    void nr4EnabledChanged(bool on);
    void mnrEnabledChanged(bool on);
    void rn2EnabledChanged(bool on);
    void rn2TxEnabledChanged(bool on);   // RN2 on the TX mic pre-amp (#2813)
    void dfnrEnabledChanged(bool on);
    void nvAfxEnabledChanged(bool on);
    void txRawPcmReady(const QByteArray& pcm);  // raw 24kHz stereo int16 PCM for RADEEngine
    // Post-final-limiter TX monitor PCM (24 kHz stereo int16) — the exact stream
    // packetised to the radio. Fires for all phone/SSB TX (unlike txRawPcmReady,
    // which is RADE-only), so it is the source for Client-Side TX recording:
    // connect to QsoRecorder::feedTxAudio (#3556). Emitted from the audio thread;
    // receivers connect via Qt::AutoConnection (queued across threads).
    //
    // `clientLeveled` is true when the frames came from an external TCI/DAX
    // client (feedDaxTxAudio's markExternalSource) rather than the mic chain or
    // the engine's own tone generators. Such a client owns its level — WSJT-X's
    // Pwr slider attenuates the audio it streams — and a host-modulating
    // backend must not run makeup gain over it (#4796). Slots that only record
    // or meter the stream can ignore the flag (Qt permits connecting to a slot
    // with fewer arguments).
    void txFinalMonitorPcmReady(const QByteArray& int16Stereo, bool clientLeveled);
    // Local CW/CWX sidetone for the Client-Side QSO recorder (#2539), 24 kHz
    // stereo int16 — the recorder's native WAV format. Pumped on the audio
    // thread while the radio is keyed for CW (no mic-driven onTxAudioReady in
    // CW). Connect to QsoRecorder::feedTxAudio.
    void cwSidetoneRecordPcmReady(const QByteArray& int16Stereo);
    // True while WE are sending CW (radio keyed + our keyer active), so the
    // recorder opens its TX gate for CW the same way moxChanged does for voice.
    // Ownership-correct: driven by our local keyer, not any-owner interlock.
    void cwRecordingActiveChanged(bool active);
    void txPacketReady(const QByteArray& vitaPacket);  // VITA-49 TX packet for PanadapterStream
    // Sidetone-tapped audio for the TX-side CW decoder (#2417).  Emitted
    // from the audio thread; receivers should connect via Qt::AutoConnection
    // (which becomes queued across threads) so feedAudio() lands on the
    // decoder's thread.  Format: 24 kHz stereo float32 — same as the
    // RX panStream::audioDataReady() path so CwDecoder::feedAudio()
    // accepts it without a separate adapter.
    void txDecodeAudioReady(const QByteArray& pcm24kStereoFloat);
    // `channels` is carried explicitly (#4489) rather than left for a consumer
    // to infer from the block's byte count — every current emit site passes 2
    // (interleaved stereo, see writeAudio()), but a consumer must not assume
    // that stays true; it must read this argument.
    void receivePresentationPostDspAudioReady(const QString& source,
                                              const QString& sourceId,
                                              const QByteArray& pcmFloat,
                                              int sampleRate,
                                              int channels);
    void receivePresentationOutputAudioReady(const QString& source,
                                             const QString& sourceId,
                                             const QByteArray& pcmStereoFloat,
                                             int sampleRate);

    void pcMicLevelChanged(float peakDbfs, float avgDbfs);  // client-side PC mic metering
    void scopeSamplesReady(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    // Emitted on the local TX audio that will feed the radio. For PC mic
    // voice this is after the strip, PC mic gain, and final limiter; for
    // DAX/TCI/RADE digital bypasses this is the pre-packetization waveform
    // because those paths intentionally skip the voice chain.
    void txPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);
    // Mirror of txPostChainScopeReady for the RX side: high-rate emit
    // (~125 Hz) so the channel strip's "Aetherial Waveform — RX" panel sees a
    // wall-clock-accurate scope.  The shared scopeSamplesReady throttles at
    // 25 ms which made the strip's RX scroll lag wall clock at short
    // time-window settings.
    //
    // LOSSY — FOR DISPLAY ONLY.  This is throttled at 8 ms and the throttle
    // DISCARDS the whole block, it does not merely skip a repaint.  Blocks
    // shorter than 8 ms are therefore dropped outright: with NR2 enabled the
    // RX drain hands over whole radio packets (5.33 ms on a Flex LAN stream)
    // microseconds apart, and only the first of each drain tick survives —
    // about half the audio.  An earlier version of this comment claimed "no
    // sample loss across audio callbacks", which holds only while blocks are
    // at least 8 ms long; Copy Assist was wired here on the strength of it and
    // was fed a stream with a gap at every phoneme (#4486).
    //
    // Anything that must see EVERY sample — recognisers, decoders, recorders —
    // belongs on receivePresentationPostDspAudioReady, which is unthrottled and
    // additionally tags its source.  Also note this signal is emitted for every
    // RX source with no tag, so a station running a Kiwi alongside the Flex
    // sees the two interleaved here.
    void rxPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);
    void tncRxAudioReady(const QByteArray& monoFloat32Pcm, int sampleRate);
    void radioTransmittingChanged(bool tx);
    void mutedChanged(bool muted);                          // local audio output mute state
    void inputDeviceChanged();
    void outputDeviceChanged();
    void txBypassChanged(bool on);                          // master TX BYPASS state
    void rxBypassChanged(bool on);                          // master RX BYPASS state
    // Fired by saveClientReverbSettings() after any reverb param
    // changes — UI surfaces (strip panel + docked applet) listen and
    // refresh from a single notification path instead of polling.
    void clientReverbStateChanged();

private slots:
    void onTxAudioReady();
    // CW-sidetone record pump tick (#2539): while the radio is keyed for CW,
    // render the local sidetone and feed it to the QSO recorder.
    void onCwRecordPump();

private:
    enum class RxAudioBuffer {
        Main,
        KiwiSdr,
    };

    enum class RxDspSource {
        Main,
        KiwiSdr,
    };

    struct ExternalRxAudioSourceState {
        QString id;
        QByteArray rxBuffer;
        std::deque<QByteArray> rxPackets;
        QByteArray outputBuffer;
        QByteArray nr2Output;
        std::unique_ptr<SpectralNR> nr2;
        std::unique_ptr<RNNoiseFilter> rn2;
#ifdef HAVE_SPECBLEACH
        std::unique_ptr<SpecbleachFilter> nr4;
#endif
#ifdef __APPLE__
        std::unique_ptr<MacNRFilter> mnr;
#endif
#ifdef HAVE_DFNR
        std::unique_ptr<DeepFilterFilter> dfnr;
#endif
#ifdef HAVE_NVIDIA_AFX
        std::unique_ptr<NvidiaAfxFilter> nvAfx;
#endif
        std::unique_ptr<Resampler> rxResampler;
        std::unique_ptr<Resampler> rxResamplerR;
        float gain{1.0f};
        int pan{50};
        int presentationDelayMs{0};
        bool enabled{false};
        bool muted{false};
        // Transmit gating is presentation-only for managed Kiwi sources:
        // the feed, jitter buffer, and DSP keep running through TX, and
        // only the final mix contribution is ramped to zero. With
        // keepAudioDuringTx set the source stays audible during TX.
        // txResumeHoldMs > 0 keeps the gate closed that long past unkey so
        // the resume lands on post-TX audio instead of the operator's own
        // delayed TX tail (default QDeadlineTimer is already expired, so
        // an unarmed deadline never holds the gate).
        bool keepAudioDuringTx{false};
        int txResumeHoldMs{0};
        QDeadlineTimer txResumeDeadline;
        float txGateGain{1.0f};
        bool prebuffering{false};
        bool dspInitializationPending{false};
    };

    struct AutomationAudioCaptureChunk {
        QString point;
        QString source;
        QString sourceId;
        int sampleRate{DEFAULT_SAMPLE_RATE};
        int channels{2};
        qint64 startNs{0};
        QByteArray pcm;
    };

    QAudioFormat makeFormat() const;
    float computeRMS(const QByteArray& pcm) const;
    QByteArray applyBoost(const QByteArray& pcm, float gain) const;
    QByteArray buildVitaTxPacket(const float* samples, int numStereoSamples);
    void sendVoiceTxPacket(const QByteArray& pcmData, quint32 streamId);
    void emitScopeFromFloat32Stereo(const QByteArray& pcm, int sampleRate, bool tx);
    void emitScopeFromInt16Stereo(const QByteArray& pcm, int sampleRate, bool tx);
    void emitTxPostChainScopeFromInt16Stereo(const QByteArray& pcm, int sampleRate);
    void emitTxPostChainScopeFromFloat32Stereo(const QByteArray& pcm, int sampleRate);
    void emitRxPostChainScopeFromFloat32Stereo(const QByteArray& pcm, int sampleRate);
    void emitTncRxTapFromFloat32Stereo(const QByteArray& pcm, int sampleRate);
    QByteArray resampleStereo(const QByteArray& pcm,
                              RxDspSource source = RxDspSource::Main,
                              ExternalRxAudioSourceState* externalSource = nullptr);
    void processRxAudioData(const QByteArray& pcm, bool emitTncTap,
                            RxAudioBuffer targetBuffer = RxAudioBuffer::Main);
    void processMixedRxAudioData(const QByteArray& pcm,
                                 RxDspSource source = RxDspSource::Main,
                                 ExternalRxAudioSourceState* externalSource = nullptr);
    void captureAutomationAudio(const QString& point,
                                const QString& source,
                                const QString& sourceId,
                                const QByteArray& pcm,
                                int sampleRate,
                                int channels);
    void processNr2(const QByteArray& stereoPcm,
                    RxDspSource source = RxDspSource::Main,
                    ExternalRxAudioSourceState* externalSource = nullptr);
    static void processNr2StereoSharedMask(SpectralNR& nr2,
                                           const float* src,
                                           int stereoFrames,
                                           QByteArray& output);
    void updateRxBufferStats();
    ExternalRxAudioSourceState* externalKiwiSource(const QString& sourceId,
                                                   bool create);
    bool kiwiSdrAudioActive() const;
    bool externalKiwiSourceProcessing(
        const ExternalRxAudioSourceState& source) const;
    bool anyExternalKiwiAudioEnabled() const;
    bool anyExternalKiwiBufferQueued() const;
    qsizetype externalKiwiOutputBufferBytes() const;
    bool ensureLegacyKiwiDspState();
    bool ensureExternalKiwiSourceDspState(const QString& sourceId);
    bool ensureAllKiwiDspState();
    void scheduleAllKiwiDspStateInitialization();
    void resetLegacyKiwiDspState();
    void clearLegacyKiwiDspState();
    void resetExternalKiwiDspState(ExternalRxAudioSourceState& source);
    void clearExternalKiwiDspState(ExternalRxAudioSourceState& source);
    std::unique_ptr<SpectralNR> createNr2Filter(
        const QString& label, bool forceLegacyGeometry = false) const;
    std::unique_ptr<RNNoiseFilter> createRn2Filter(const QString& label) const;
    RNNoiseFilter* rn2ForSource(RxDspSource source,
                                ExternalRxAudioSourceState* externalSource) const;
#ifdef HAVE_SPECBLEACH
    std::unique_ptr<SpecbleachFilter> createNr4Filter(const QString& label) const;
    SpecbleachFilter* nr4ForSource(
        RxDspSource source,
        ExternalRxAudioSourceState* externalSource) const;
#endif
#ifdef __APPLE__
    std::unique_ptr<MacNRFilter> createMnrFilter(const QString& label) const;
    MacNRFilter* mnrForSource(RxDspSource source,
                              ExternalRxAudioSourceState* externalSource) const;
#endif
#ifdef HAVE_DFNR
    std::unique_ptr<DeepFilterFilter> createDfnrFilter(const QString& label) const;
    DeepFilterFilter* dfnrForSource(
        RxDspSource source,
        ExternalRxAudioSourceState* externalSource) const;
#endif
#ifdef HAVE_NVIDIA_AFX
    std::unique_ptr<NvidiaAfxFilter> createNvAfxFilter(const QString& label) const;
    NvidiaAfxFilter* nvAfxForSource(
        RxDspSource source,
        ExternalRxAudioSourceState* externalSource) const;
#endif
    // RX comp operates on the post-Gate float32 stereo buffer.
    void applyClientCompRxFloat32(QByteArray& float32);
    // RX gate operates on the post-EQ float32 stereo buffer.
    void applyClientGateRxFloat32(QByteArray& float32);
    // RX de-esser operates on the post-Comp float32 stereo buffer (#2425).
    void applyClientDeEssRxFloat32(QByteArray& float32);
    // RX tube operates on the post-Comp float32 stereo buffer.
    void applyClientTubeRxFloat32(QByteArray& float32);
    // RX pudu operates on the post-Tube float32 stereo buffer.
    void applyClientPuduRxFloat32(QByteArray& float32);

    void accumulatePcMicMeterInt16Stereo(const QByteArray& int16stereo);
    void logTxInputChannelDiagnostics(const TxMicChannelNormalizer::Diagnostics& diagnostics,
                                      const char* route);
    static TxCaptureHealthTracker::CaptureState txCaptureState(QAudio::State state);
    qint64 txCaptureBufferedBytes() const;
    qint64 txCaptureBufferCapacityBytes() const;
    qint64 txCaptureNowMs() const;
    bool tciAudioFresh() const;
    void pumpWsprBeacon();
    void feedDaxTxAudioInternal(const QByteArray& float32pcm,
                                bool markExternalSource,
                                bool forceRadioDaxRoute);
    void observeTxCaptureState(QAudio::State state);
    // Overload for callers that must sample the unread depth before draining it.
    void observeTxCaptureState(QAudio::State state, qint64 bufferedBytes);
    void recordTxCaptureLocalTxAttempt();
    void noteTxCaptureBacklogDiscard(qint64 discardedBytes);
    void logTxCaptureHealthEvent(TxCaptureHealthTracker::Event event);
    void logTxCaptureHealthSummary(const QString& reason, bool anomaly);

    // Apply the whole RX DSP chain in the configured order.  Phase 0
    // ships the dispatcher with no implemented stages — every entry is
    // a no-op until its class lands.  Plays float32 stereo (the native
    // RX format after NR).
    void applyClientRxDspFloat32(QByteArray& float32);

    // RX
    QAudioSink*   m_audioSink{nullptr};
    QPointer<QIODevice> m_audioDevice;   // sink-owned device, may vanish on hot-unplug

    // Dedicated low-latency sink for the local CW sidetone — kept separate
    // from the RX sink so the RX path keeps its 100 ms jitter cushion.
    // Backend chosen at start time: PortAudio when HAVE_PORTAUDIO and not
    // disabled by AppSettings["CwSidetoneBackend"]=="QAudioSink"; QAudioSink
    // (push mode, 2 ms timer, 50 ms buffer) otherwise.  See CwSidetoneSinkBackend.h.
    std::unique_ptr<class CwSidetoneSinkBackend> m_sidetoneSink;
    std::unique_ptr<QuindarLocalSink>            m_quindarLocalSink;

    // TX
    QUdpSocket    m_txSocket;
    QAudioSource* m_audioSource{nullptr};
    QPointer<QIODevice> m_micDevice;
#ifdef Q_OS_MAC
    QTimer*       m_txPollTimer{nullptr};
    QBuffer*      m_micBuffer{nullptr};
#endif
    QHostAddress  m_txAddress;
    quint16       m_txPort{0};
    quint32       m_txStreamId{0};         // DAX TX stream
    quint32       m_remoteTxStreamId{0};  // remote_audio_tx (voice/VOX)
    // Host-modulating backend (HL2): no Flex stream id will ever be assigned,
    // so the TX gate keys off this instead. setHostModulation() is the single
    // write path.
    bool          m_hostModulation{false};
    quint8        m_txPacketCount{0};    // 4-bit, mod 16
    QByteArray    m_txAccumulator;       // accumulate PCM until 128 stereo pairs
    QByteArray    m_voxAccumulator;     // accumulate PCM for VOX/met_in_rx stream
    QByteArray    m_txFloatAccumulator;  // accumulate float32 PCM for RADE modem TX
    QByteArray    m_daxPreTxBuffer;      // short rolling pre-TX buffer for low-latency DAX mode
    std::atomic<bool> m_radeMode{false}; // RADE digital voice mode active (atomic: cross-thread)
    std::atomic<float> m_pcMicGain{1.0f};     // client-side PC mic gain (0.0-1.0)
    std::atomic<bool>  m_daxTxMode{false};    // DAX TX mode: VirtualAudioBridge handles TX
    QElapsedTimer      m_txSourceStartTime;
    quint64            m_txLifecycleGeneration{0};
    QElapsedTimer      m_txCaptureHealthClock;
    TxCaptureHealthTracker m_txCaptureHealth;
    // WASAPI silent-open watchdog (#2929): some endpoints accept an open they
    // cannot honour — Qt returns a non-null QIODevice that then delivers no
    // bytes. The watchdog reopens along AudioFormatNegotiator::silentOpenLadder()
    // if nothing arrives within ~1.5 s.
    //
    // The ladder walks channel count AND sample format. One bool could express
    // "mono retry used up" while mono was the only recovery; it cannot express a
    // position in a multi-rung ladder, which is why a Float-first capture could
    // strand an Int16-capable mic (review of PR #5017).
    bool               m_txReceivedAnyBytes{false};
    // Set by the watchdog, consumed by the next startTxStream(): distinguishes a
    // recovery reopen (advance the ladder) from a fresh start (reset to stage 0).
    bool               m_txSilentOpenRetryArmed{false};
    // Position in AudioFormatNegotiator::txOpenLadder(m_txSilentOpenInitialChannels),
    // the ONE ordered rate x format x channels sequence. 0 == the initial,
    // non-recovery open. Advanced by BOTH failure shapes — a null open moves
    // it inline, a non-null/no-data open moves it from the watchdog — so the
    // two can no longer disagree about which rung is open (round-3 review of
    // PR #5017). Forward-only, which is what makes a tuple already observed
    // silent unreachable rather than merely unlikely.
    int                m_txOpenStage{0};
    // Channel count of the stage-0 open, so the ladder stays stable across
    // reopens even though fmt.channelCount() changes underneath it.
    int                m_txSilentOpenInitialChannels{2};
#ifdef Q_OS_MAC
    std::atomic<bool>  m_allowBluetoothTelephonyOutput{false};
#endif
    std::atomic<bool>  m_daxTxUseRadioRoute{false}; // false = low-latency route (dax=0)
    std::atomic<bool>  m_transmitting{false}; // true when radio is in TX AND we own TX
    std::atomic<bool>  m_radioTransmitting{false}; // true when radio is in TX (any owner)
    std::atomic<bool>  m_opusTxEnabled{false}; // Opus TX encoding for SmartLink
    std::unique_ptr<class OpusCodec> m_opusTxCodec; // lazy-init on first TX with Opus
    QByteArray    m_opusTxAccumulator;  // accumulate stereo samples for Opus frame
    OpusTxPacer   m_opusTxPacer;
    QTimer*       m_opusTxPaceTimer{nullptr};
    QElapsedTimer m_opusTxPaceClock;
    QElapsedTimer m_opusTxDropLogTimer;
    quint64       m_opusTxDropsSinceLog{0};

    // Client-side PC mic metering (accumulated over ~50ms window)
    float         m_pcMicPeak{0.0f};
    double        m_pcMicSumSq{0.0};
    int           m_pcMicSampleCount{0};
    static constexpr int kMicMeterWindowSamples = 24000 / 20;  // ~50ms at 24kHz

    QElapsedTimer m_lastRxScopeEmit;
    QElapsedTimer m_lastTxScopeEmit;
    QElapsedTimer m_lastTxPostChainScopeEmit;
    QElapsedTimer m_lastRxPostChainScopeEmit;
    QByteArray    m_scopeRxScratch;
    QByteArray    m_scopeTxScratch;
    QByteArray    m_scopeTxPostChainScratch;
    QByteArray    m_scopeRxPostChainScratch;
    QByteArray    m_tncRxTapScratch;

    QAudioDevice m_outputDevice;
    QAudioDevice m_inputDevice;
    std::atomic<float> m_rxVolume{1.0f};
    std::atomic<bool>  m_rxBoost{false};  // 50% software gain boost (#1445)
    std::atomic<float> m_rxOutputTrimDb{0.0f};  // ±12 dB linear trim, post-boost
    std::atomic<int>   m_rxPan{50};       // 0=left, 50=centre, 100=right (#1460)
    std::atomic<int>   m_rxBufferCapMs{100}; // RX buffer cap in ms (#1505; default lowered 200->100 for #3193)
    std::atomic<bool>  m_muted{false};
    // RX sink device rate (negotiated via AudioFormatNegotiator). Audio is
    // resampled from the 24k canonical rate up to this when they differ (#3306).
    // Atomic: written from startRxStream() (GUI thread) and read from the RX
    // drain timer lambda plus several status/scope read paths — matches the
    // surrounding std::atomic<int> neighbours (m_rxPan, m_rxBufferCapMs).
    std::atomic<int> m_rxOutputRate{DEFAULT_SAMPLE_RATE};
    std::unique_ptr<Resampler> m_rxResampler;       // 24k→device rate, L channel (lazy init)
    std::unique_ptr<Resampler> m_rxResamplerR;      // 24k→device rate, R channel — kept in sync with m_rxResampler
    std::unique_ptr<Resampler> m_radeRxResampler;   // separate 24k→device rate for RADE decoded speech
    std::unique_ptr<CwSidetoneGenerator> m_cwSidetone;  // local CW sidetone, mixed into RX drain
    // Second, recorder-only CW sidetone generator at the 24 kHz recorder rate.
    // Keyed in lockstep with m_cwSidetone via setCwKeyDown(); the CW record pump
    // (onCwRecordPump) renders it to the QSO recorder while the radio is keyed
    // for CW, so a Client-Side recording carries the operator's sent CW/CWX
    // (#2539). Always enabled at a fixed level so it records even when the
    // audible sidetone is off/low (operator monitoring CW via the radio).
    std::unique_ptr<CwSidetoneGenerator> m_cwRecordSidetone;
    std::vector<float> m_cwRecordSidetoneScratch;       // int16<->float render scratch
    // CW record pump state (#2539). The pump free-runs on the audio thread;
    // m_cwKeyedThisOver latches when our keyer fires in a CW mode (set in
    // setCwKeyDown, aged out by the pump via cwLatchShouldAge — NOT reset on
    // the radio TX→RX edge, which break-in drops in every inter-element gap,
    // #4281) so the pump excludes voice/DAX/tune overs that never key the
    // sidetone. m_cwPumpElapsed drives wall-clock-accurate frame counts so
    // morse timing in the recording matches real time.
    QTimer*            m_cwRecordPump{nullptr};
    QElapsedTimer      m_cwPumpElapsed;
    bool               m_cwPumpActive{false};            // audio-thread only
    std::atomic<bool>  m_cwKeyedThisOver{false};
    // Our keyer fired AND the radio actually keyed at some point in this over.
    // The pump is free-running (startCwRecordPump is invoked once and never
    // stopped), and ownership deliberately ignores the interlock so break-in
    // gaps do not end the over — which together meant a key-down that never
    // transmitted (TX-inhibited radio, disconnected, CWX local sidetone
    // feedback) took the recorder's TX slot and could auto-start a recording.
    // Latching the interlock rather than sampling it keeps gaps covered while
    // requiring that a transmission happened at all (#4281).
    std::atomic<bool>  m_cwOverHadTx{false};
    // steady_clock ns of our last CW key EDGE — down or up. Written by every
    // keyer path, read by the pump to age the latch out. 0 = never keyed.
    // Both edges are stamped deliberately: stamping only key-down measured the
    // element's own duration as part of the gap, so the over ran long by up to
    // 3 units. See cwOverHangMs() and onCwRecordPump (#4281).
    std::atomic<int64_t> m_cwLastKeyEdgeNs{0};
    // The inputs cwOverTxActive() composes so the over machinery tracks OUR
    // transmission, not the whole radio's (#4281): the interlock's
    // tx_client_handle attribution (radios that report no handle parse as
    // ours — RadioModel's permissive default, so the composite degrades to
    // any-owner rather than dropping our own over), the TX slice's CW-mode
    // mirror, and the tune-carrier mirror.
    std::atomic<bool>  m_radioTxOwnedByUs{true};
    std::atomic<bool>  m_txModeIsCw{false};
    std::atomic<bool>  m_tuneActive{false};
    // Local keyer speed, mirrored from TransmitModel::cwSpeed so the pump can
    // size the over-hang in dit units rather than wall-clock milliseconds.
    std::atomic<int>   m_cwWpm{20};
    // How long after the last key edge the CW over is considered finished.
    //
    // It must outlast the longest silence WITHIN an over — the inter-word gap,
    // 7 dit units — and no longer, because for its whole duration the recorder
    // holds RX audio off (QsoRecorder::feedRxAudio) so the pump's silence is
    // not interleaved with receive audio. Every extra millisecond here is a
    // millisecond of the other station's reply missing from the recording, and
    // in QSK they answer within 200-400 ms.
    //
    // 8 units = the inter-word gap plus one unit of margin for keyer jitter.
    // At 20 WPM that is 480 ms; at 30 WPM, 320 ms. A fixed 1500 ms (the first
    // version of this fix) cost 1.5 s of deafness after EVERY over at every
    // speed, which is a regression against the recorder's own purpose.
    //
    // Known limitation: Farnsworth sending stretches word gaps beyond 7 units
    // at the character speed this reports, so a Farnsworth over may split at a
    // word boundary. That degrades to the pre-fix behaviour for one gap rather
    // than losing audio.
    //
    // The arithmetic lives in CwRecordGate.h so it is pinned by the same test
    // as the ownership rule.
    int64_t cwOverHangMs() const {
        return AetherSDR::cwOverHangMs(
            m_cwWpm.load(std::memory_order_relaxed),
            m_cwOverWpm.load(std::memory_order_relaxed));
    }
    // Slowest speed announced for the CURRENT over, 0 = none (a paddle over).
    // Min-tracked by noteCwOverSpeed as CWX segments announce at send time;
    // cleared by the pump when the over's latch ages out and on the CW-mode
    // exit edge, so the next paddle over is back on the mirror alone (#4281).
    std::atomic<int>   m_cwOverWpm{0};
    std::atomic<bool>  m_qsoRecordingActive{false};   // a recording file is open
    // Atomic gate for the TX-side CW decode tap (#2417).  Flipped from
    // MainWindow on MOX / CwDecodeTxEnabled changes; checked on the
    // sidetone audio thread so the mirror lambda can return cheaply
    // when TX-decode is off.
    std::atomic<bool> m_cwDecodeTxTapEnabled{false};
    std::atomic<bool> m_tncRxTapEnabled{false};
    bool  m_radeTxNeedsResample{false};  // RADE: input rate != 24 kHz
    bool  m_txInputMono{false};          // TX: legacy convenience mirror of m_txInputChannels == 1
    int   m_txInputChannels{2};          // TX: actual negotiated input channel count
    int   m_txInputRate{24000};          // TX: actual input sample rate
    // TX: actual negotiated input sample format. Float32 now leads the mic
    // ladder on Windows and Linux, so the 48 kHz float voice strip is fed
    // without an Int16 round trip; Int16 remains the next rung there, and stays
    // the macOS default — the macOS preferred-RATE rung leads with the per-OS
    // format order rather than the device's preferred format, so CoreAudio
    // reporting Float does not silently reorder it (AudioFormatNegotiator.cpp).
    // A Float32-only virtual driver still lands on Float via the preferredFormat
    // catch-all rung (#1090). The support snapshot must report what was
    // negotiated rather than assume either case.
    QAudioFormat::SampleFormat m_txInputFormat{QAudioFormat::Int16};
    TxMicChannelNormalizer::ChannelMode m_txMicChannelMode{
        TxMicChannelNormalizer::ChannelMode::Auto};
    TxMicChannelNormalizer::AutoState m_txMicChannelState;
    TxMicChannelNormalizer::ChannelMode m_daxRadioTxChannelMode{
        TxMicChannelNormalizer::ChannelMode::Auto};
    TxMicChannelNormalizer::AutoState m_daxRadioTxChannelState;
    QElapsedTimer m_lastTxMicChannelLog;
    QElapsedTimer m_lastDaxRadioChannelLog;
    std::unique_ptr<Resampler> m_txResampler;  // RADE e.g. 48k -> 24k (lazy init)

    // DSP lifecycle mutex: held during feedAudioData() DSP section AND
    // during enable/disable to prevent use-after-free (#502)
    mutable std::recursive_mutex m_dspMutex;
    quint64 m_dspConfigurationGeneration{0};

    // Client-side NR2 (spectral)
    std::unique_ptr<SpectralNR> m_nr2;
    std::unique_ptr<SpectralNR> m_kiwiSdrNr2;
    std::atomic<bool> m_nr2Enabled{false};
    // Set true while the connected MAIN source is the demo (SimBackend), whose
    // 128-sample frames need the original 256/2 NR2 geometry (see createNr2Filter).
    // This is scoped to the main filter only — real radios and Kiwi keep the
    // 1024/4 geometry.
    std::atomic<bool> m_mainSourceLegacyNr2{false};
    // Client-side NR4 (libspecbleach)
#ifdef HAVE_SPECBLEACH
    std::unique_ptr<SpecbleachFilter> m_nr4;
    std::unique_ptr<SpecbleachFilter> m_kiwiSdrNr4;
#endif
    std::atomic<bool> m_nr4Enabled{false};

    // Client-side MNR (macOS MMSE-Wiener)
#ifdef __APPLE__
    std::unique_ptr<MacNRFilter> m_mnr;
    std::unique_ptr<MacNRFilter> m_kiwiSdrMnr;
#endif
    std::atomic<bool>  m_mnrEnabled{false};
    std::atomic<float> m_mnrStrength{1.0f};

    // Client-side RN2 (RNNoise)
    std::unique_ptr<RNNoiseFilter> m_rn2;
    std::unique_ptr<RNNoiseFilter> m_kiwiSdrRn2;
    std::atomic<bool> m_rn2Enabled{false};

    // Client-side RN2 — TX path (mic pre-amp).  Lazy-allocated under
    // m_dspMutex on toggle-on, freed on toggle-off.  Mirrors the RX
    // RN2 ownership pattern above.  (#2813)
    std::unique_ptr<RNNoiseFilter> m_rn2Tx;
    std::atomic<bool> m_rn2TxEnabled{false};

    // Client-side DFNR (DeepFilterNet3)
#ifdef HAVE_DFNR
    std::unique_ptr<DeepFilterFilter> m_dfnr;
    std::unique_ptr<DeepFilterFilter> m_kiwiSdrDfnr;
#endif
    std::atomic<bool> m_dfnrEnabled{false};

    // Optional NVIDIA AFX GPU denoiser (runtime-loaded; flag always present so
    // mutual-exclusion in the other NR setters compiles regardless of the build).
#ifdef HAVE_NVIDIA_AFX
    std::unique_ptr<NvidiaAfxFilter> m_nvAfx;
    std::unique_ptr<NvidiaAfxFilter> m_kiwiSdrNvAfx;
#endif
    std::atomic<bool> m_nvAfxEnabled{false};

    // Client-side parametric EQ, independent instances for RX and TX.
    std::unique_ptr<ClientEq> m_clientEqRx;
    std::unique_ptr<ClientEq> m_clientEqTx;
    // Client-side TX compressor (Pro-XL-style).
    std::unique_ptr<ClientComp> m_clientCompTx;
    std::unique_ptr<ClientComp> m_clientCompRx;
    // Client-side TX downward expander / noise gate.
    std::unique_ptr<ClientGate> m_clientGateTx;
    std::unique_ptr<ClientGate> m_clientGateRx;
    // Client-side TX de-esser.
    std::unique_ptr<ClientDeEss> m_clientDeEssTx;
    std::unique_ptr<ClientDeEss> m_clientDeEssRx;
    // Client-side TX tube saturator.
    std::unique_ptr<ClientTube> m_clientTubeTx;
    std::unique_ptr<ClientTube> m_clientTubeRx;
    // Client-side TX PUDU exciter.
    std::unique_ptr<ClientPudu> m_clientPuduTx;
    std::unique_ptr<ClientPudu> m_clientPuduRx;
    // Client-side TX reverb.
    std::unique_ptr<ClientReverb> m_clientReverbTx;
    std::unique_ptr<ClientFinalLimiter> m_clientFinalLimiterTx;
    std::unique_ptr<ClientTxTestTone>   m_clientTxTestTone;
    std::unique_ptr<WsprBeacon>         m_wsprBeacon;
    std::unique_ptr<ClientQuindarTone>  m_clientQuindarTone;
    std::unique_ptr<TxVoiceProcessor>   m_txVoiceProcessor;
    // Audio-thread only: prevents duplicate queued recovery work after the
    // impossible matched-egress frame-count mismatch.
    bool m_txVoiceEgressRecoveryQueued{false};
    // Audio-thread-loaded pointer for the post-final-limiter monitor
    // (final-output recording).  Same lock-free atomic pointer pattern
    // as m_txPostDspMonitor.
    std::atomic<ClientPuduMonitor*> m_txFinalMonitor{nullptr};
    // Post-DSP TX monitor — owned by MainWindow; we just hold a
    // pointer the audio thread can load lock-free per block.
    std::atomic<ClientPuduMonitor*> m_txPostDspMonitor{nullptr};
    // Generalised TX DSP chain — stages packed one-byte-per-slot into
    // a uint64_t so the audio thread can load the full order in a
    // single atomic read per block.  TxChainStage::None terminates the
    // list; unused slots are zero.  Default canonical order:
    // [Gate, Eq, DeEss, Comp, Tube, Enh, Reverb].
    std::atomic<uint64_t> m_txChainPacked{0};
    // Master-bypass snapshot — the stages that were enabled at the
    // moment setTxBypassed(true) was called.  Used solely to restore
    // exactly that set when bypass is released; m_txBypassActive is
    // the source of truth for "currently bypassed" (see issue #2892:
    // clicking BYPASS when all stages are already disabled yields an
    // empty snapshot, so the snapshot alone cannot represent state).
    QVector<TxChainStage> m_txBypassSnapshot;
    // RN2 TX is functionally a chain member but lives outside the
    // user-orderable TxChainStage enum (it runs ahead of the chain in
    // onTxAudioReady — see #2813), so BYPASS has to snapshot/restore it
    // alongside the stages or RN2 keeps processing while every visible
    // stage is bypassed (#3054).
    bool m_txBypassSnapshotRn2{false};
    bool m_txBypassActive{false};
    // RX chain — same packing convention.  RxChainStage::None terminates
    // the list.  Phase 0 dispatcher iterates this but every stage is a
    // no-op until its DSP class lands in a later phase.
    std::atomic<uint64_t> m_rxChainPacked{0};
    // RX bypass snapshot — sibling of m_txBypassSnapshot.  Used solely
    // to restore the stages that were enabled when bypass engaged;
    // m_rxBypassActive is the source of truth for "currently bypassed".
    QVector<RxChainStage> m_rxBypassSnapshot;
    // RX RN2 lives in the NR cluster, not the RxChainStage enum, but
    // BYPASS must still suppress it so the post-bypass RX path is truly
    // transparent (#3054).  Same parallel-bool pattern as TX above.
    bool m_rxBypassSnapshotRn2{false};
    bool m_rxBypassActive{false};
    // Scratch buffer for in-place EQ on the RX path (avoids per-call alloc).
    QByteArray m_clientEqRxScratch;
    QByteArray m_clientCompRxScratch;
    QByteArray m_clientGateRxScratch;
    QByteArray m_clientDeEssRxScratch;
    QByteArray m_clientTubeRxScratch;
    QByteArray m_clientPuduRxScratch;
    // Post-EQ analyzer tap. One ring per path, mono (L+R averaged).
    // Audio thread writes via tapClientEqRxStereo() / tapClientEqTxFloat32();
    // UI thread snapshots via the public
    // copyRecent*() accessors. Mutex is held for microseconds only.
    mutable std::mutex m_clientEqTapMutex;
    float              m_clientEqTapRx[kClientEqTapSize]{};
    float              m_clientEqTapTx[kClientEqTapSize]{};
    int                m_clientEqTapRxWrite{0};
    int                m_clientEqTapTxWrite{0};
    void tapClientEqRxStereo(const float* stereoInterleaved, int frames);
    void tapClientEqTxFloat32(const float* f32, int samples, int channels);

    // Pre-allocated NR2 output buffers (avoid per-call heap allocation)
    QByteArray m_nr2Output;
    QByteArray m_kiwiSdrNr2Output;

    // Zombie sink watchdog: tracks consecutive RX timer ticks where we have
    // data to write but bytesFree() == 0, indicating a stale WASAPI handle.
    // After ~2 seconds (200 ticks × 10ms), force a restart. (#1361)
    int m_rxZombieTickCount{0};
    static constexpr int kZombieTickThreshold = 200;  // 200 × 10ms = 2s

    // Audio liveness watchdog: detects when audio data stops arriving while
    // the RX stream is still active (e.g. CoreAudio silently discarding
    // data after extended idle, or radio stops sending VITA-49 packets).
    // Restarts the RX stream after ~15 seconds of silence. (#1411)
    QElapsedTimer m_lastAudioFeedTime;
    static constexpr qint64 kAudioLivenessTimeoutMs = 15000;

    // TCI client active-audio gate: started on every feedDaxTxAudio() frame
    // so onTxAudioReady() can step the local mic capture aside while a TCI
    // client is feeding TX audio. Otherwise both paths emit txPacketReady
    // concurrently and the higher-rate mic stream drowns out the TCI tone
    // — most visible on macOS, where the default CoreAudio input is a real
    // webcam mic that produces continuous ambient packets.
    QElapsedTimer m_tciAudioTimer;
    static constexpr qint64 kTciAudioActiveWindowMs = 200;
    QTimer* m_wsprPumpTimer{nullptr};
    QElapsedTimer m_wsprPumpClock;
    qint64 m_wsprPumpedFrames{0};
    QByteArray m_wsprFloatScratch;
    // DAX TX mode borrowed for the duration of a WSPR frame so the mic path
    // cannot produce a second packet stream against the same m_txPacketCount.
    // m_wsprSavedDaxTxMode makes start/stop idempotent — stopWsprPump() has
    // several early-return callers.
    bool m_wsprPreviousDaxTxMode{false};
    bool m_wsprSavedDaxTxMode{false};

    // Stale session watchdog: detects when audio data is being written but
    // processedUSecs() hasn't advanced, indicating the WASAPI session is
    // silently discarding audio (e.g. after Teams/Zoom reconfigures the
    // audio endpoint). Restarts after ~3 seconds of stale output. (#1569)
    qint64 m_lastProcessedUSecs{0};
    int    m_rxStaleTickCount{0};
    static constexpr int kStaleTickThreshold = 300;  // 300 × 10ms = 3s

    // RX audio buffer handling
    QTimer*       m_rxTimer{nullptr};
    QByteArray    m_rxBuffer;  // normal Flex RX source audio at 24 kHz, pre-DSP
    std::deque<QByteArray> m_rxPackets;  // whole Flex packets for NR2 packet-mode
    QByteArray    m_kiwiSdrRxBuffer;  // decoded Kiwi source audio at 24 kHz, pre-DSP
    std::deque<QByteArray> m_kiwiSdrRxPackets;  // whole Kiwi packets for NR2 packet-mode
    QByteArray    m_rxOutputBuffer;  // post-DSP speaker audio at output device rate
    QByteArray    m_kiwiSdrOutputBuffer;  // post-DSP Kiwi speaker audio at output device rate
    QByteArray    m_radeRxBuffer;  // decoded RADE speech at output device rate
    std::atomic<bool> m_kiwiSdrAudioEnabled{false};
    bool m_legacyKiwiDspInitializationPending{false};
    QFutureSynchronizer<void> m_dspInitializationTasks;
    std::mutex m_dspInitializationTasksMutex;
    bool m_dspInitializationStopping{false};
    std::atomic<bool> m_kiwiSdrAudioTransmitMuted{false};
    // Audio-thread-only. TX mix gate for the delayed-Flex presentation path
    // (mirrors ExternalRxAudioSourceState::txGateGain): with a Receive Sync
    // delay applied, the presentation buffer holds pre-key-down RX audio
    // that must ramp out of the mix at key-down instead of playing through
    // the start of the transmission.
    float             m_flexTxGateGain{1.0f};
    std::atomic<int>  m_flexReceivePresentationDelayMs{0};
    std::atomic<int>  m_kiwiReceivePresentationDelayMs{0};
    QString           m_externalKiwiReceivePresentationDelaySourceId;
    int               m_externalKiwiReceivePresentationDelayMs{0};
    std::atomic<bool> m_rxPresentationPrebuffering{false};
    std::atomic<bool> m_kiwiSdrPrebuffering{false};
    std::vector<std::unique_ptr<ExternalRxAudioSourceState>> m_externalKiwiSources;
    std::atomic<qsizetype> m_rxBufferBytes{0};
    std::atomic<qsizetype> m_rxBufferPeakBytes{0};
    std::atomic<quint64>   m_rxBufferUnderrunCount{0};
    std::atomic<int>       m_rxBufferSampleRate{DEFAULT_SAMPLE_RATE};
    std::atomic<int>       m_rxPlaybackQueuedMs{0};
    // Cached on the audio thread; diagnostics may read from GUI/automation.
    std::atomic<int>       m_receivePresentationPlaybackQueuedMs{0};
    std::atomic<int>       m_receivePresentationFlexRawBufferMs{0};
    std::atomic<int>       m_receivePresentationFlexOutputBufferMs{0};
    std::atomic<int>       m_receivePresentationKiwiSdrRawBufferMs{0};
    std::atomic<int>       m_receivePresentationKiwiSdrOutputBufferMs{0};
    std::atomic<int>       m_receivePresentationExternalKiwiRawBufferMs{0};
    std::atomic<int>       m_receivePresentationExternalKiwiOutputBufferMs{0};
    std::atomic<quint64>   m_receivePresentationOutputSignalEmitCount{0};
    std::atomic<quint64>   m_receivePresentationOutputSignalSuppressedCount{0};
    mutable std::mutex     m_automationAudioCaptureMutex;
    std::atomic<bool>      m_automationAudioCaptureActive{false};
    bool                   m_automationCaptureRaw{false};
    bool                   m_automationCapturePost{false};
    bool                   m_automationCaptureOutput{false};
    bool                   m_automationCaptureFinal{false};
    qint64                 m_automationCaptureStartNs{0};
    qint64                 m_automationCaptureEndNs{0};
    qsizetype              m_automationCaptureBytes{0};
    qsizetype              m_automationCaptureMaxBytes{0};
    QVector<AutomationAudioCaptureChunk> m_automationCaptureChunks;
    static constexpr int   kKiwiSdrJitterTargetMs = 360;
    static constexpr int   kKiwiSdrBufferCapMs = 1000;
    static constexpr int   kKiwiSdrTxGateRampMs = 8;
    void resetRxChainStateForSourceSwitch();
    std::unique_ptr<Resampler> m_kiwiSdrRxResampler;
    std::unique_ptr<Resampler> m_kiwiSdrRxResamplerR;

    // VITA-49 TX constants
    static constexpr int    TX_SAMPLES_PER_PACKET = 128;  // audio frames per packet
    static constexpr int    TX_PCM_BYTES_PER_PACKET = TX_SAMPLES_PER_PACKET * 2 * 2; // 128 frames × 2ch × int16
    static constexpr int    VITA_HEADER_WORDS = 7;
    static constexpr int    VITA_HEADER_BYTES = VITA_HEADER_WORDS * 4;  // 28 bytes
    static constexpr quint32 FLEX_OUI = 0x001C2D;
    static constexpr quint16 FLEX_INFO_CLASS = 0x534C;
    static constexpr quint16 PCC_IF_NARROW = 0x03E3;
    static constexpr quint16 PCC_DAX_REDUCED = 0x0123;  // reduced BW DAX (24kHz int16 mono)
};

} // namespace AetherSDR
