#pragma once

#include <QObject>
#include <QAudioSink>
#include <QAudioSource>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QIODevice>
#include <QUdpSocket>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QPointer>

#include "TxMicChannelNormalizer.h"

class QMediaDevices;

#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

namespace AetherSDR {

class SpectralNR;
class SpecbleachFilter;
class RNNoiseFilter;
class NvidiaBnrFilter;
class DeepFilterFilter;
class Resampler;
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
    Q_INVOKABLE void stopTxStream();

    // Set the DAX TX stream ID (from radio's response to "stream create type=dax_tx")
    void setTxStreamId(quint32 id) { m_txStreamId = id; }
    quint32 txStreamId() const { return m_txStreamId; }
    // Set the remote audio TX stream ID (for voice TX and VOX monitoring)
    void setRemoteTxStreamId(quint32 id) { m_remoteTxStreamId = id; }

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
    // Normally the radio handles panning, but client-side NR mono-mixes
    // L+R, discarding the balance. This re-applies it to the NR output
    // so that the pan slider still works when NR2/NR4/etc. are active (#1460).
    void setRxPan(int panValue);
    int  rxPan() const { return m_rxPan.load(); }
    void  setRxBufferCapMs(int ms) { m_rxBufferCapMs.store(qBound(50, ms, 1000)); }
    int   rxBufferCapMs() const { return m_rxBufferCapMs.load(); }

    bool isMuted() const       { return m_muted.load(); }
    void setMuted(bool m);
    bool isRxStreaming() const { return m_audioSink != nullptr; }
    bool isTxStreaming() const { return m_audioSource != nullptr; }

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
    void setRadioTransmitting(bool tx);  // raw interlock state (regardless of TX ownership)
    void clearTxAccumulators() { m_txAccumulator.clear(); m_txFloatAccumulator.clear(); m_daxPreTxBuffer.clear(); }
    Q_INVOKABLE void feedDaxTxAudio(const QByteArray& float32pcm);

    // Plays RADE decoded speech (int16 stereo 24kHz) bypassing m_radeMode block
    void feedDecodedSpeech(const QByteArray& pcm);

    // Client-side NR2 (spectral noise reduction)
    // Q_INVOKABLE: called from main thread, runs on audio worker thread (#502)
    Q_INVOKABLE void setNr2Enabled(bool on);
    bool nr2Enabled() const { return m_nr2Enabled.load(); }
    // NR2 user-adjustable parameters (thread-safe via atomic in SpectralNR)
    void setNr2GainMax(float v);
    void setNr2Qspp(float v);
    void setNr2GainSmooth(float v);
    void setNr2GainMethod(int method);
    void setNr2NpeMethod(int method);
    void setNr2AeFilter(bool on);
    // Client-side RN2 (RNNoise neural noise suppression)
    Q_INVOKABLE void setRn2Enabled(bool on);
    bool rn2Enabled() const { return m_rn2Enabled.load(); }

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

    // Client-side BNR (NVIDIA NIM GPU noise removal)
    Q_INVOKABLE void setBnrEnabled(bool on);
    bool bnrEnabled() const { return m_bnrEnabled.load(); }
    void setBnrAddress(const QString& addr);
    QString bnrAddress() const { return m_bnrAddress; }
    void setBnrIntensity(float ratio);
    float bnrIntensity() const;
    bool bnrConnected() const;

    // Client-side DFNR (DeepFilterNet3 neural noise reduction)
    Q_INVOKABLE void setDfnrEnabled(bool on);
    bool dfnrEnabled() const { return m_dfnrEnabled.load(); }
    void setDfnrAttenLimit(float db);
    float dfnrAttenLimit() const;
    void setDfnrPostFilterBeta(float beta);

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

    // Generalised TX DSP chain — each stage is a separate processing
    // block run in order on the TX audio path.  Only Eq and Comp are
    // implemented today; the remaining stages are placeholders for
    // Phase 2+ work (Gate, DeEss, Tube, Enh from #1661) and are no-ops
    // until their DSP classes ship.
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
    static void generateWisdom(std::function<void(int,int,const std::string&)> progress = nullptr);

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

    // Local CW sidetone generator — accessor used by RadioModel signal
    // routing and PhoneCwApplet UI bindings.
    CwSidetoneGenerator* cwSidetone() { return m_cwSidetone.get(); }

public slots:
    // Receives stripped PCM from PanadapterStream::audioDataReady().
    void feedAudioData(const QByteArray& pcm);

signals:
    void rxStarted();
    void rxStopped();
    void levelChanged(float rms);  // audio level for VU meter, 0.0–1.0
    void nr2EnabledChanged(bool on);
    void nr4EnabledChanged(bool on);
    void mnrEnabledChanged(bool on);
    void rn2EnabledChanged(bool on);
    void bnrEnabledChanged(bool on);
    void bnrConnectionChanged(bool connected);
    void dfnrEnabledChanged(bool on);
    void txRawPcmReady(const QByteArray& pcm);  // raw 24kHz stereo int16 PCM for RADEEngine
    void txPacketReady(const QByteArray& vitaPacket);  // VITA-49 TX packet for PanadapterStream

    void pcMicLevelChanged(float peakDbfs, float avgDbfs);  // client-side PC mic metering
    void scopeSamplesReady(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    // Emitted on every TX block AFTER everything the strip can do to
    // the audio: user DSP chain, PC mic gain, and the final brickwall
    // limiter.  This is the exact int16 stream that gets packetised
    // into VITA-49 and sent to the radio — what the strip's
    // "Waveform CE-SSB" panel listens to so the operator sees the
    // actual transmitted envelope.
    void txPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);
    // Mirror of txPostChainScopeReady for the RX side: high-rate emit
    // (~125 Hz, no sample loss across audio callbacks) so the channel
    // strip's "Aetherial Waveform — RX" panel sees a wall-clock-accurate
    // scope.  The shared scopeSamplesReady throttles at 25 ms which made
    // the strip's RX scroll lag wall clock at short time-window settings.
    void rxPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);
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

private:
    QAudioFormat makeFormat() const;
    float computeRMS(const QByteArray& pcm) const;
    QByteArray applyBoost(const QByteArray& pcm, float gain) const;
    QByteArray buildVitaTxPacket(const float* samples, int numStereoSamples);
    void sendVoiceTxPacket(const QByteArray& pcmData, quint32 streamId);
    void emitScopeFromFloat32Stereo(const QByteArray& pcm, int sampleRate, bool tx);
    void emitScopeFromInt16Stereo(const QByteArray& pcm, int sampleRate, bool tx);
    void emitTxPostChainScopeFromInt16Stereo(const QByteArray& pcm, int sampleRate);
    void emitRxPostChainScopeFromFloat32Stereo(const QByteArray& pcm, int sampleRate);
    QByteArray resampleStereo(const QByteArray& pcm);
    void processNr2(const QByteArray& stereoPcm);
    void updateRxBufferStats();
    // Apply client-side TX EQ in-place. No-op if disabled. Caller owns data.
    void applyClientEqTxInt16(QByteArray& int16stereo);
    void applyClientEqTxFloat32(QByteArray& float32);
    // Apply client-side TX compressor in-place.  No-op if disabled.
    void applyClientCompTxInt16(QByteArray& int16stereo);
    void applyClientCompTxFloat32(QByteArray& float32);
    // RX comp operates on the post-Gate float32 stereo buffer.
    void applyClientCompRxFloat32(QByteArray& float32);
    // Apply client-side TX gate in-place.  No-op if disabled.
    void applyClientGateTxInt16(QByteArray& int16stereo);
    void applyClientGateTxFloat32(QByteArray& float32);
    // RX gate operates on the post-EQ float32 stereo buffer.
    void applyClientGateRxFloat32(QByteArray& float32);
    // Apply client-side TX de-esser in-place.  No-op if disabled.
    void applyClientDeEssTxInt16(QByteArray& int16stereo);
    void applyClientDeEssTxFloat32(QByteArray& float32);
    // RX de-esser operates on the post-Comp float32 stereo buffer (#2425).
    void applyClientDeEssRxFloat32(QByteArray& float32);
    // Apply client-side TX tube saturator in-place.  No-op if disabled.
    void applyClientTubeTxInt16(QByteArray& int16stereo);
    void applyClientTubeTxFloat32(QByteArray& float32);
    // RX tube operates on the post-Comp float32 stereo buffer.
    void applyClientTubeRxFloat32(QByteArray& float32);
    // Apply client-side TX PUDU exciter in-place.  No-op if disabled.
    void applyClientPuduTxInt16(QByteArray& int16stereo);
    void applyClientPuduTxFloat32(QByteArray& float32);
    // RX pudu operates on the post-Tube float32 stereo buffer.
    void applyClientPuduRxFloat32(QByteArray& float32);
    // Apply client-side TX reverb in-place.  No-op if disabled.
    void applyClientReverbTxInt16(QByteArray& int16stereo);
    void applyClientReverbTxFloat32(QByteArray& float32);
    void applyClientFinalLimiterTxInt16(QByteArray& int16stereo);
    void applyClientFinalLimiterTxFloat32(QByteArray& float32);
    // Apply the whole TX DSP chain (CMP + EQ) in the configured order.
    void applyClientTxDspInt16(QByteArray& int16stereo);
    void applyClientTxDspFloat32(QByteArray& float32);

    void accumulatePcMicMeterInt16Stereo(const QByteArray& int16stereo);
    void logTxInputChannelDiagnostics(const TxMicChannelNormalizer::Diagnostics& diagnostics,
                                      const char* route);

    // Apply the whole RX DSP chain in the configured order.  Phase 0
    // ships the dispatcher with no implemented stages — every entry is
    // a no-op until its class lands.  Plays float32 stereo (the native
    // RX format after NR).
    void applyClientRxDspFloat32(QByteArray& float32);

    // RX
    QAudioSink*   m_audioSink{nullptr};
    QPointer<QIODevice> m_audioDevice;   // sink-owned device, may vanish on hot-unplug

    // Dedicated low-latency sink for the local CW sidetone — kept separate
    // from the RX sink so the RX path keeps its 200 ms jitter cushion.
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
#ifdef Q_OS_MAC
    std::atomic<bool>  m_allowBluetoothTelephonyOutput{false};
#endif
    std::atomic<bool>  m_daxTxUseRadioRoute{false}; // false = low-latency route (dax=0)
    std::atomic<bool>  m_transmitting{false}; // true when radio is in TX AND we own TX
    std::atomic<bool>  m_radioTransmitting{false}; // true when radio is in TX (any owner)
    std::atomic<bool>  m_opusTxEnabled{false}; // Opus TX encoding for SmartLink
    std::unique_ptr<class OpusCodec> m_opusTxCodec; // lazy-init on first TX with Opus
    QByteArray    m_opusTxAccumulator;  // accumulate stereo samples for Opus frame
    QVector<QByteArray> m_opusTxQueue;  // pacing queue for even 10ms packet delivery
    QTimer*       m_opusTxPaceTimer{nullptr};

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

    QAudioDevice m_outputDevice;
    QAudioDevice m_inputDevice;
    std::atomic<float> m_rxVolume{1.0f};
    std::atomic<bool>  m_rxBoost{false};  // 50% software gain boost (#1445)
    std::atomic<float> m_rxOutputTrimDb{0.0f};  // ±12 dB linear trim, post-boost
    std::atomic<int>   m_rxPan{50};       // 0=left, 50=centre, 100=right (#1460)
    std::atomic<int>   m_rxBufferCapMs{200}; // RX buffer cap in ms (#1505)
    std::atomic<bool>  m_muted{false};
    bool  m_resampleTo48k{false};      // RX: upsample 24kHz → 48kHz output
    std::unique_ptr<Resampler> m_rxResampler;       // 24k→48k for L channel (lazy init)
    std::unique_ptr<Resampler> m_rxResamplerR;      // 24k→48k for R channel — kept in sync with m_rxResampler
    std::unique_ptr<Resampler> m_radeRxResampler;   // separate 24k→48k for RADE decoded speech
    std::unique_ptr<CwSidetoneGenerator> m_cwSidetone;  // local CW sidetone, mixed into RX drain
    bool  m_txNeedsResample{false};      // TX: input rate != 24kHz, needs resampling
    bool  m_txInputMono{false};          // TX: legacy convenience mirror of m_txInputChannels == 1
    int   m_txInputChannels{2};          // TX: actual negotiated input channel count
    int   m_txInputRate{24000};          // TX: actual input sample rate
    TxMicChannelNormalizer::ChannelMode m_txMicChannelMode{
        TxMicChannelNormalizer::ChannelMode::Auto};
    TxMicChannelNormalizer::AutoState m_txMicChannelState;
    TxMicChannelNormalizer::ChannelMode m_daxRadioTxChannelMode{
        TxMicChannelNormalizer::ChannelMode::Auto};
    TxMicChannelNormalizer::AutoState m_daxRadioTxChannelState;
    QElapsedTimer m_lastTxMicChannelLog;
    QElapsedTimer m_lastDaxRadioChannelLog;
    std::unique_ptr<Resampler> m_txResampler;  // e.g. 48k→24k (lazy init)

    // DSP lifecycle mutex: held during feedAudioData() DSP section AND
    // during enable/disable to prevent use-after-free (#502)
    std::recursive_mutex m_dspMutex;

    // Client-side NR2 (spectral)
    std::unique_ptr<SpectralNR> m_nr2;
    std::atomic<bool> m_nr2Enabled{false};
    // Client-side NR4 (libspecbleach)
#ifdef HAVE_SPECBLEACH
    std::unique_ptr<SpecbleachFilter> m_nr4;
#endif
    std::atomic<bool> m_nr4Enabled{false};

    // Client-side MNR (macOS MMSE-Wiener)
#ifdef __APPLE__
    std::unique_ptr<MacNRFilter> m_mnr;
#endif
    std::atomic<bool>  m_mnrEnabled{false};
    std::atomic<float> m_mnrStrength{1.0f};

    // Client-side RN2 (RNNoise)
    std::unique_ptr<RNNoiseFilter> m_rn2;
    std::atomic<bool> m_rn2Enabled{false};

    // Client-side BNR (NVIDIA NIM)
    std::unique_ptr<NvidiaBnrFilter> m_bnr;
    std::unique_ptr<Resampler> m_bnrUp;    // 24k→48k mono
    std::unique_ptr<Resampler> m_bnrDown;  // 48k→24k mono
    std::atomic<bool> m_bnrEnabled{false};
    QString m_bnrAddress{"localhost:8001"};
    QByteArray m_bnrOutBuf;  // jitter buffer: denoised 24kHz stereo int16
    bool m_bnrPrimed{false}; // true after enough denoised data accumulated
    void processBnr(const QByteArray& stereoPcm);

    // Client-side DFNR (DeepFilterNet3)
#ifdef HAVE_DFNR
    std::unique_ptr<DeepFilterFilter> m_dfnr;
#endif
    std::atomic<bool> m_dfnrEnabled{false};

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
    std::unique_ptr<ClientQuindarTone>  m_clientQuindarTone;
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
    // [Gate, Eq, DeEss, Comp, Tube, Enh] — but only Eq and Comp have
    // implementations today, so the others are no-op pass-throughs.
    std::atomic<uint64_t> m_txChainPacked{0};
    // Master-bypass snapshot — the stages that were enabled at the
    // moment setTxBypassed(true) was called.  Lives on the engine so
    // both the Chain applet and the channel-strip BYPASS buttons
    // share one source of truth.  Empty ⇒ not bypassed.
    QVector<TxChainStage> m_txBypassSnapshot;
    // RX chain — same packing convention.  RxChainStage::None terminates
    // the list.  Phase 0 dispatcher iterates this but every stage is a
    // no-op until its DSP class lands in a later phase.
    std::atomic<uint64_t> m_rxChainPacked{0};
    // RX bypass snapshot — sibling of m_txBypassSnapshot.  Empty ⇒ not
    // bypassed; non-empty ⇒ holds the stages that were enabled when
    // bypass engaged so we can restore exactly that set on un-bypass.
    QVector<RxChainStage> m_rxBypassSnapshot;
    // Scratch buffer for in-place EQ on the RX path (avoids per-call alloc).
    QByteArray m_clientEqRxScratch;
    QByteArray m_clientEqTxScratch;
    QByteArray m_clientCompTxScratch;
    QByteArray m_clientCompRxScratch;
    QByteArray m_clientGateTxScratch;
    QByteArray m_clientGateRxScratch;
    QByteArray m_clientDeEssTxScratch;
    QByteArray m_clientDeEssRxScratch;
    QByteArray m_clientTubeTxScratch;
    QByteArray m_clientTubeRxScratch;
    QByteArray m_clientPuduTxScratch;
    QByteArray m_clientPuduRxScratch;
    QByteArray m_clientReverbTxScratch;
    QByteArray m_clientFinalLimiterTxScratch;
    // Post-EQ analyzer tap. One ring per path, mono (L+R averaged).
    // Audio thread writes via tapClientEqRxStereo() / tapClientEqTxInt16()
    // / tapClientEqTxFloat32(); UI thread snapshots via the public
    // copyRecent*() accessors. Mutex is held for microseconds only.
    mutable std::mutex m_clientEqTapMutex;
    float              m_clientEqTapRx[kClientEqTapSize]{};
    float              m_clientEqTapTx[kClientEqTapSize]{};
    int                m_clientEqTapRxWrite{0};
    int                m_clientEqTapTxWrite{0};
    void tapClientEqRxStereo(const float* stereoInterleaved, int frames);
    void tapClientEqTxInt16(const int16_t* int16stereo, int frames);
    void tapClientEqTxFloat32(const float* f32, int samples, int channels);

    // Pre-allocated NR2 work buffers (avoid per-call heap allocation)
    std::vector<float> m_nr2Mono;
    std::vector<float> m_nr2Processed;
    QByteArray m_nr2Output;

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

    // Stale session watchdog: detects when audio data is being written but
    // processedUSecs() hasn't advanced, indicating the WASAPI session is
    // silently discarding audio (e.g. after Teams/Zoom reconfigures the
    // audio endpoint). Restarts after ~3 seconds of stale output. (#1569)
    qint64 m_lastProcessedUSecs{0};
    int    m_rxStaleTickCount{0};
    static constexpr int kStaleTickThreshold = 300;  // 300 × 10ms = 3s

    // RX audio buffer handling
    QTimer*       m_rxTimer{nullptr};
    QByteArray    m_rxBuffer;
    QByteArray    m_radeRxBuffer;  // decoded RADE speech; mixed into drain output, never appended to m_rxBuffer
    std::atomic<qsizetype> m_rxBufferBytes{0};
    std::atomic<qsizetype> m_rxBufferPeakBytes{0};
    std::atomic<quint64>   m_rxBufferUnderrunCount{0};
    std::atomic<int>       m_rxBufferSampleRate{DEFAULT_SAMPLE_RATE};

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
