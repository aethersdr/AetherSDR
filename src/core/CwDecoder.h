#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QThread>

#include <atomic>
#include <memory>
#include <mutex>

namespace AetherSDR {

// Client-side CW (Morse code) decoder using ggmorse.
// Runs decoding on a worker thread. Feed it 24kHz stereo float32 PCM
// and it emits decoded text character by character.
//
// Usage:
//   decoder.start();
//   connect(audioSource, &Source::audioReady, &decoder, &CwDecoder::feedAudio);
//   connect(&decoder, &CwDecoder::textDecoded, panel, &Panel::appendText);

class CwDecoder : public QObject {
    Q_OBJECT

public:
    explicit CwDecoder(QObject* parent = nullptr);
    ~CwDecoder() override;

    // Lifecycle and parameter setters run on this QObject's owning thread.
    // feedAudio() may run on the audio producer thread.
    void start();
    void stop();
    bool isRunning() const { return m_running; }

    float estimatedPitch() const { return m_pitch; }
    float estimatedSpeed() const { return m_speed; }

    // Lock pitch/speed to current detected values (prevents wandering)
    void lockPitch(bool lock);
    void lockSpeed(bool lock);
    void setPitchRange(int minHz, int maxHz);
    void setSpeedRange(int minWpm, int maxWpm);

    // Force pitch + speed to specific values and lock both — used by the
    // TX-side decoder (#2417) where the operator's keying parameters are
    // known from PhoneCwApplet rather than detected from the audio.
    // Changes are applied by the worker before the next frame; subsequent
    // calls are no-ops if the requested values are unchanged.
    void setKnownParameters(float pitchHz, float speedWpm);
    bool isPitchLocked() const { return m_pitchLocked; }
    bool isSpeedLocked() const { return m_speedLocked; }

public slots:
    // Feed 24kHz stereo float32 PCM (same format as AudioEngine receives).
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void textDecoded(const QString& text, float cost);
    void statsUpdated(float pitchHz, float speedWpm);

private:
    void decodeLoop();
    std::unique_ptr<QThread> m_workerThread;

    // One coherent pending configuration. Only setters and the decoder worker
    // take this mutex; feedAudio() never does. GGMorse itself is worker-local.
    struct DecodeParameters {
        float pitchHz{-1.0f};
        float speedWpm{-1.0f};
        float pitchRangeMin{500.0f};
        float pitchRangeMax{700.0f};
        float speedRangeMin{-1.0f};
        float speedRangeMax{-1.0f};
    };
    std::mutex m_parametersMutex;
    DecodeParameters m_pendingParameters;
    bool m_parametersDirty{true};

    // Ring buffer for audio samples (mono int16 at 24kHz)
    QMutex        m_bufMutex;
    QByteArray    m_ringBuf;
    static constexpr int RING_CAPACITY = 24000 * 2 * 4; // 4 seconds of mono int16

    std::atomic<bool> m_running{false};
    std::atomic<float> m_pitch{0};
    std::atomic<float> m_speed{0};
    std::atomic<bool> m_pitchLocked{false};
    std::atomic<bool> m_speedLocked{false};
};

} // namespace AetherSDR
