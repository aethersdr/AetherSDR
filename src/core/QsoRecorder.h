#pragma once

#include "QsoRecordStartPolicy.h"
#include "PcmFrame.h"
#include "QsoRecordingFormat.h"
#include "QsoPcmConverter.h"
#include "QsoWavPlayback.h"

#include <QAudio>
#include <QAudioDevice>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

class QAudioSink;
class QAudioFormat;

namespace AetherSDR {

class SliceModel;
class TransmitModel;
class QsoRecorderWriteErrorTestAccess;
class QsoRecorderRatesTestAccess;
class QsoRecorderPlaybackTestAccess;

// Records QSO audio (both RX and TX sides) to WAV files.
//
// Usage:
//   - Connect feedRxFrame() (typed RX) to RadioModel::rxDemodAudioReady
//   - Connect feedTxAudio() (int16 post-limiter TX monitor) to
//     AudioEngine::txFinalMonitorPcmReady — the source that carries SSB/phone TX
//     (txRawPcmReady is RADE-only and would leave SSB recordings silent, #3556)
//   - Connect onMoxChanged() to TransmitModel::moxChanged()
//   - Set the active slice for frequency/mode metadata via setSlice()
//
// While transmitting, the radio mutes the RX stream, so feedRxAudio() would
// otherwise write full-length silence. Writes are MOX-gated: RX is written only
// while receiving, the TX monitor only while transmitting, producing a single
// time-interleaved RX/TX file that matches Radio-Side recording (#3556).
//
// Recording triggers:
//   - Auto: starts when MOX goes true (first TX), stops after idle timeout
//   - Manual: startRecording() / stopRecording()
//
// A start can be REFUSED — see QsoRecordStartPolicy.h. startRecording() then
// emits recordingBlocked() and creates no file at all. Callers must not assume
// a start succeeded; check isRecording() (this class lives below the UI seam
// and cannot show a dialog itself, so the GUI owns the operator-facing message).
//
// Output: immutable PCM16 stereo WAV, 24/48 kHz from current RX metadata at
// start; legacy24 when none is available. Voice/CW remain fixed24 inputs.

class QsoRecorder : public QObject {
    Q_OBJECT

    friend class QsoRecorderWriteErrorTestAccess;
    friend class QsoRecorderRatesTestAccess;
    friend class QsoRecorderPlaybackTestAccess;

public:
    explicit QsoRecorder(QObject* parent = nullptr);
    ~QsoRecorder() override;

    // Configuration
    void setRecordingDir(const QString& path);
    QString recordingDir() const { return m_recordingDir; }

    void setIdleTimeoutSecs(int secs);
    int idleTimeoutSecs() const { return m_idleTimeoutSecs; }

    void setAutoRecordEnabled(bool on);
    bool autoRecordEnabled() const { return m_autoRecord; }

    void setCallsign(const QString& call);
    QString callsign() const { return m_callsign; }

    // Audio output device the playback sink opens.  When null, falls back
    // to QMediaDevices::defaultAudioOutput().  MainWindow's AudioOutputRouter
    // seeds this from AudioEngine::outputDevice() at registration and refreshes
    // it on AudioEngine::outputDeviceChanged so QSO playback follows the user's
    // selection in Radio Settings > Audio rather than going to the system
    // default (#3361 / #3306).
    void setOutputDevice(const QAudioDevice& dev) { m_outputDevice = dev; }

    // Filename component toggles
    void setIncludeDate(bool on) { m_includeDate = on; }
    void setIncludeTime(bool on) { m_includeTime = on; }
    void setIncludeFrequency(bool on) { m_includeFreq = on; }
    void setIncludeMode(bool on) { m_includeMode = on; }

    // Active slice (provides frequency + mode for filename)
    void setSlice(SliceModel* slice);

    bool isRecording() const { return m_recording; }
    bool isPlaying() const { return m_playing; }
    bool hasLastRecording() const { return !m_lastRecordingPath.isEmpty(); }
    // The last finalized recording decoded to `format` -- what "TX Playback"
    // sends to the transmitter. The recorder owns its file layout, so the
    // caller never opens the WAV itself. Empty, with `error` filled, when
    // there is no recording or it cannot be read.
    std::optional<QByteArray> lastRecordingPcm(const QAudioFormat& format,
                                               QString* error = nullptr,
                                               qint64 maxFrames = kQsoPlaybackMaxFrames,
                                               bool prefixOnly = false) const;

    // Answers "does the connected backend demodulate in-process?"
    // (IRadioBackend::ownsRxAudio) for the start policy. A CALLBACK, not a
    // cached bool, deliberately: it is read fresh on every start, so a backend
    // swap cannot leave a stale flag behind — and a stale flag failing open is
    // the exact shape of #4629. Unset (the default) reads as false, which is
    // right for a Flex and for no radio at all.
    void setBackendOwnsRxAudioProvider(std::function<bool()> provider)
    {
        m_backendOwnsRxAudio = std::move(provider);
    }

    // Would startRecording() be allowed right now? Reads the same live settings
    // and the same provider startRecording() does, so callers that want to ask
    // BEFORE committing to a UI state change (the automation bridge, wanting a
    // reason for its reply) get exactly the answer the guard will give. No
    // side effects.
    RecordStartDecision evaluateStart() const;

    // Path of the in-progress recording (while recording) else the last
    // finalized one; empty if neither. Used by the automation bridge to locate
    // the WAV for capture-file verification.
    QString recordingFilePath() const {
        // Lock: m_file is mutated/deleteLater'd under m_writeMutex by the feed
        // path and finalizeFile(); reading it unlocked races those and can hit
        // a half-torn-down handle (UAF). All callers are external (automation),
        // none hold the write lock, so this can't self-deadlock.
        std::lock_guard<std::mutex> lock(m_writeMutex);
        return m_file ? m_file->fileName() : m_lastRecordingPath;
    }

    // Duration of current recording in seconds (0 if not recording)
    int recordingDurationSecs() const;

public slots:
    // Manual control
    void startRecording();
    // Finalized PCM duration, including the finite conversion tail; zero if idle.
    int stopRecording();

    // Playback of last recording
    void startPlayback();
    void stopPlayback();

    // Serialized audio feeds. Fixed-format compatibility RX is float32 stereo
    // 24 kHz; TX/CW are native int16 stereo 24 kHz with separate histories.
    // feedRxAudio has NO production caller since typed RX landed — MainWindow
    // wires rxDemodAudioReady straight to feedRxFrame. It is retained as the
    // fixed-rate compatibility seam and is exercised by the recorder tests;
    // the legacy-RX guard in feedFixedPcm is correct but no longer live.
    void feedRxAudio(const QByteArray& pcm);
    void feedTxAudio(const QByteArray& pcm);
    // Observes current speaker metadata even while stopped or TX-gated. The
    // observed block advances the replay cursor but is never replayed at start.
    void feedRxFrame(const AetherSDR::PcmFrame& frame);
    void feedCwAudio(const QByteArray& pcm);

    // TX state tracking (connect to TransmitModel::moxChanged)
    void onMoxChanged(bool mox);
    // A CW over has started/finished, from AudioEngine::cwRecordingActiveChanged.
    // Separate from onMoxChanged because break-in gives no MOX edge that spans the
    // over: the interlock toggles once per ELEMENT (measured on a FLEX-8400 at
    // 20 WPM: 47 edges in 15.8 s). Routing this through onMoxChanged let raw MOX
    // shut the gate in every inter-element gap (#4281).
    void setCwOverActive(bool active);

private:
    // Auto-record + idle-timer bookkeeping shared by a voice over (onMoxChanged)
    // and a CW over (setCwOverActive). Split out so the CW path cannot write
    // m_transmitting, which MOX alone owns (#4281).
    void applyOverBookkeeping(bool overActive);
public:

signals:
    void recordingStarted(const QString& filePath);
    void recordingStopped(const QString& filePath, int durationSecs);
    void recordingError(const QString& error);
    // A start was refused before any file was created (#4629). Carries the
    // REASON, not prose: the operator-facing wording (and its tr()) belongs to
    // the GUI, and src/core must not depend on QtWidgets — see
    // .github/workflows/engine-boundary.yml EB1/EB2.
    void recordingBlocked(AetherSDR::RecordStartDecision reason);
    void playbackStarted();
    void playbackStopped();
    void muteRxRequested(bool mute);  // mute live RX during playback

private slots:
    void onPlaybackSinkState(QAudio::State state);

private:
    // Whether finalizeFile() may raise the zero-capture diagnostic. Silent is
    // for the destructor — see the comment there.
    enum class FinalizeReport { Diagnose, Silent };

    // Who asked. A refusal is reported EVERY time for a deliberate act (the
    // operator pressed record and is owed an answer), but only on the first of
    // a repeating run for auto-record — which retries on every MOX rising edge
    // and would otherwise raise one notification per key-down for as long as
    // the condition lasts.
    enum class StartTrigger { Manual, Auto };
    void beginRecording(StartTrigger trigger);

    // Last refusal already reported for an auto-record attempt; cleared when a
    // recording actually starts or the decision changes, so a genuinely new
    // problem is never swallowed.
    std::optional<RecordStartDecision> m_lastAutoBlocked;

    void startFile();
    int finalizeFile(FinalizeReport report = FinalizeReport::Diagnose);
    void finalizeWriteFailure(quint64 generation);
    QString buildFilename() const;
    static QString sanitizeForPath(const QString& s);
    bool writeWavHeader();
    bool patchWavHeader();
    qint64 writeFile(const char* data, qint64 size);
    bool seekFile(qint64 position);
    bool flushFile();
    void queueWriteFailure(const QString& detail);
    bool preparePlaybackPcm(const QAudioFormat& sinkFormat, QString& error);
    void startPlaybackWithFormat(const QAudioDevice& device, const QAudioFormat& format);
    QAudio::Error startPlaybackSink(const QAudioDevice& device, const QAudioFormat& format);
    void releasePlaybackSink(bool stop);

    enum class PcmSource { None, TypedRx, LegacyRx, Voice, Cw };
    void feedFixedPcm(const QByteArray& pcm, PcmSource source);
    bool selectPcmSegment(PcmSource source, PcmFormat format,
                          const PcmFrame& frame = {});
    bool finishPcmSegment();
    bool writeConvertedPcm(const QByteArray& pcm);

    // All fields below share m_writeMutex, including idle metadata observation.
    // The bounded observation retains at most one owning frame/epoch. Its gate
    // persists across files so stop/start cannot admit replayed queued blocks.
    PcmFrameGate m_rxGate;
    PcmFrame m_rxObservation;
    // Latches the 'ignoring a second speaker producer' warning to once per
    // selected source, since RX frames arrive continuously.
    bool     m_foreignSourceWarned{false};
    std::optional<QsoRecordingFormat> m_fileFormat;
    PcmSource m_pcmSource{PcmSource::None};
    PcmFrame m_pcmEpoch;
    quint64 m_nextRxSample{0};
    std::unique_ptr<QsoPcmConverter> m_pcmConverter;

    // Recording state
    std::atomic<bool> m_recording{false};  // checked lock-free on the audio feed fast path
    std::atomic<bool> m_transmitting{false};  // MOX state; gates RX vs TX writes (#3556)
    // True for the whole of a CW over that OUR keyer is sending. ORed with
    // m_transmitting on both feed paths so an over survives the interlock
    // toggling between elements under break-in (#4281).
    std::atomic<bool> m_cwOverActive{false};
    QFile*      m_file{nullptr};
    QDateTime   m_startTime;
    quint32     m_dataBytes{0};    // PCM data bytes written (for WAV header patching)
    std::atomic<bool> m_writeFailurePending{false};
    std::atomic<quint64> m_recordingGeneration{0};
    QString m_pendingWriteError;

    // Configuration
    QString     m_recordingDir;
    int         m_idleTimeoutSecs{120};  // 2 minutes default
    bool        m_autoRecord{false};
    QString     m_callsign;
    bool        m_includeDate{true};
    bool        m_includeTime{true};
    bool        m_includeFreq{true};
    bool        m_includeMode{true};

    // Slice metadata (captured at recording start). QPointer auto-nulls when the
    // SliceModel is destroyed (slice removal / reconnect prune), so startFile()'s
    // guard can't dereference a freed pointer (#4003).
    QPointer<SliceModel> m_slice;
    double      m_freqMhz{0.0};
    QString     m_mode;

    // Idle timeout
    QTimer*     m_idleTimer{nullptr};

    // Playback
    bool         m_playing{false};
    quint64      m_playbackGeneration{0};
    QString      m_lastRecordingPath;
    QAudioSink*  m_playSink{nullptr};
    QBuffer      m_playBuffer;
    QByteArray   m_playPcm;
    QAudioDevice m_outputDevice;

    // Tests replace only sink operations after production format negotiation.
    // File preparation, buffer lifetime, state and mute signals remain real.
    std::function<QAudio::Error(QIODevice&, const QAudioFormat&)> m_startPlaybackSinkForTest;
    std::function<void(bool)> m_releasePlaybackSinkForTest;

    // Thread safety for audio feed paths
    mutable std::mutex  m_writeMutex;

    // Narrow deterministic seam for the recorder's real QFile operations.
    // The test uses it to make a post-open write, seek, or flush fail without
    // changing filename allocation or relying on a full filesystem. Production
    // paths leave all three unset and call QFile directly.
    std::function<qint64(QFile&, const char*, qint64)> m_writeForTest;
    std::function<bool(QFile&, qint64)> m_seekForTest;
    std::function<bool(QFile&)> m_flushForTest;
    // Deterministic revocation point after conversion, before write admission.
    // Installed before test threads start; unset in production.
    std::function<void()> m_beforePcmWriteForTest;

    // See setBackendOwnsRxAudioProvider(). Null until MainWindow installs it,
    // and null reads as false — the Flex answer, and the safe one.
    std::function<bool()> m_backendOwnsRxAudio;

    static constexpr int WAV_HEADER_SIZE = 44;
};

} // namespace AetherSDR

// recordingBlocked carries this by value. Every connection today is direct
// (same thread), which needs no registration — but a queued or cross-thread
// connection would fail at RUNTIME with an unregistered type, which is a poor
// way to find out. Declared here rather than in QsoRecordStartPolicy.h so that
// header stays free of Qt entirely and its unit test can link without it.
Q_DECLARE_METATYPE(AetherSDR::RecordStartDecision)
