#pragma once

#include <QAudio>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTimer>
#include <mutex>

class QAudioSink;

namespace MasterSDR {

class SliceModel;
class TransmitModel;

// Records QSO audio (both RX and TX sides) to WAV files.
//
// Usage:
//   - Connect feedRxAudio() to AudioEngine::feedAudioData() (or post-DSP tap)
//   - Connect feedTxAudio() to AudioEngine::txRawPcmReady()
//   - Connect onMoxChanged() to TransmitModel::moxChanged()
//   - Set the active slice for frequency/mode metadata via setSlice()
//
// Recording triggers:
//   - Auto: starts when MOX goes true (first TX), stops after idle timeout
//   - Manual: startRecording() / stopRecording()
//
// Output: 24 kHz stereo int16 WAV (matches AudioEngine native format).

class QsoRecorder : public QObject {
    Q_OBJECT

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

    // Duration of current recording in seconds (0 if not recording)
    int recordingDurationSecs() const;

public slots:
    // Manual control
    void startRecording();
    void stopRecording();

    // Playback of last recording
    void startPlayback();
    void stopPlayback();

    // Audio feeds — thread-safe, called from audio thread
    void feedRxAudio(const QByteArray& pcm);
    void feedTxAudio(const QByteArray& pcm);

    // TX state tracking (connect to TransmitModel::moxChanged)
    void onMoxChanged(bool mox);

signals:
    void recordingStarted(const QString& filePath);
    void recordingStopped(const QString& filePath, int durationSecs);
    void recordingError(const QString& error);
    void playbackStarted();
    void playbackStopped();
    void muteRxRequested(bool mute);  // mute live RX during playback

private slots:
    void onPlaybackSinkState(QAudio::State state);

private:
    void startFile();
    void finalizeFile();
    QString buildFilename() const;
    void writeWavHeader();
    void patchWavHeader();
    bool preparePlaybackPcm(int sinkRateHz);

    // Recording state
    bool        m_recording{false};
    QFile*      m_file{nullptr};
    QDateTime   m_startTime;
    quint32     m_dataBytes{0};    // PCM data bytes written (for WAV header patching)

    // Configuration
    QString     m_recordingDir;
    int         m_idleTimeoutSecs{120};  // 2 minutes default
    bool        m_autoRecord{false};
    QString     m_callsign;
    bool        m_includeDate{true};
    bool        m_includeTime{true};
    bool        m_includeFreq{true};
    bool        m_includeMode{true};

    // Slice metadata (captured at recording start)
    SliceModel* m_slice{nullptr};
    double      m_freqMhz{0.0};
    QString     m_mode;

    // Idle timeout
    QTimer*     m_idleTimer{nullptr};

    // Playback
    bool         m_playing{false};
    QString      m_lastRecordingPath;
    QAudioSink*  m_playSink{nullptr};
    QBuffer      m_playBuffer;
    QByteArray   m_playPcm;

    // Thread safety for audio feed paths
    std::mutex  m_writeMutex;

    // WAV format constants (matching AudioEngine native format)
    static constexpr int SAMPLE_RATE = 24000;
    static constexpr int NUM_CHANNELS = 2;
    static constexpr int BITS_PER_SAMPLE = 16;
    static constexpr int WAV_HEADER_SIZE = 44;
};

} // namespace MasterSDR
