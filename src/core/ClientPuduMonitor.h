#pragma once

#include <QAudio>
#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <cstdint>

class QAudioSink;

namespace MasterSDR {

// PUDU monitor — captures up to 30 seconds of post-DSP TX audio (the
// output of the full client-side PooDoo™ chain) into an in-memory
// buffer, then plays it back through the RX sink so the user can hear
// what their chain is producing without keying the radio.  On stop a
// WAV snapshot is dropped into /tmp for offline inspection; playback
// itself reads the in-memory buffer, never the file.
//
// Threading:
//  - feedTxPostDsp() runs on the audio worker thread.  Single writer,
//    lock-free via atomics.  Bails out early when not recording.
//  - Everything else (start/stop, playback tick, signals) runs on the
//    UI thread.  Audio thread hands off via Qt queued invoke when the
//    30-second cap is reached.
class ClientPuduMonitor : public QObject {
    Q_OBJECT

public:
    explicit ClientPuduMonitor(QObject* parent = nullptr);
    ~ClientPuduMonitor() override = default;

    // UI-thread transitions.  All idempotent — calling start while
    // already started, or stop while already stopped, is a no-op.
    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();

    bool isRecording()  const noexcept { return m_recording.load(std::memory_order_acquire); }
    bool isPlaying()    const noexcept { return m_playing; }
    bool hasRecording() const noexcept { return m_recordedBytes > 0; }
    int  recordedMs()   const noexcept;

    // Audio thread — appends int16 stereo 24 kHz PCM into the buffer
    // while recording.  No-op otherwise.  Stops itself and queues the
    // UI-thread auto-stop handler once the 30-s cap is reached.
    void feedTxPostDsp(const QByteArray& int16stereo) noexcept;

    // Sample-rate / channel-count assumed throughout the class.  Kept
    // as constants so the WAV writer and playback chunk sizes can
    // reference them directly.
    static constexpr int kSampleRate  = 24000;
    static constexpr int kChannels    = 2;
    static constexpr int kBytesPerFrame = kChannels * 2;
    static constexpr int kMaxSeconds  = 30;
    static constexpr int kMaxBytes    = kMaxSeconds * kSampleRate * kBytesPerFrame;

signals:
    // State transitions for the UI.
    void recordingStarted();
    void recordingStopped(int durationMs);
    void playbackStarted();
    void playbackStopped();
    // Ask MainWindow to toggle the live RX audio feed off/on.  True
    // = disconnect live RX (so we don't hear over our capture or
    // playback), false = restore.  Held from recordingStarted
    // through playbackStopped as one continuous mute window.
    void muteRxRequested(bool mute);

private slots:
    // Posted from the audio thread when the 30-s buffer fills.
    void onAutoStop();
    // Fires when the dedicated playback sink finishes draining.
    void onPlaybackSinkState(QAudio::State state);

private:
    void writeWavFile();
    // Materialise the captured buffer at the sink's native rate so
    // QAudioSink's pull-mode can consume it directly.  Returns true
    // on success with m_playPcm populated.
    bool preparePlaybackPcm(int sinkRateHz);

    // ── Audio-thread-visible state ──────────────────────────────────
    std::atomic<bool>     m_recording{false};
    std::atomic<int>      m_writeBytes{0};
    // Buffer is pre-sized to kMaxBytes at construction; the audio
    // thread memcpys into it at m_writeBytes.  Data ownership stays
    // with this object — never reassigned.
    QByteArray            m_buffer;

    // ── UI-thread-only state ────────────────────────────────────────
    int                   m_recordedBytes{0};     // snapshot of m_writeBytes after stop
    int                   m_recordedMs{0};
    bool                  m_playing{false};
    QElapsedTimer         m_recElapsed;

    // Dedicated playback pipeline.  Owns its own QAudioSink so the
    // monitor is fully decoupled from the RX sink's resample path and
    // timer cadence — sink's pull-mode consumes at its native rate,
    // which means macOS/Windows CoreAudio/WASAPI glitches from
    // timer jitter go away.  m_playPcm holds the full captured
    // buffer pre-converted to the sink's preferred rate.
    QAudioSink*            m_playSink{nullptr};
    QPointer<QIODevice>    m_playSource;
    QByteArray             m_playPcm;
    QBuffer                m_playBuffer;
};

} // namespace MasterSDR
