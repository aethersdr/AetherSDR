#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QThread>

#include <atomic>
#include <memory>

class GGMorse;

namespace MasterSDR {

// Client-side CW (Morse code) decoder using ggmorse.
// Runs decoding on a worker thread. Feed it 24kHz stereo int16 PCM
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

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    float estimatedPitch() const { return m_pitch; }
    float estimatedSpeed() const { return m_speed; }

    // Lock pitch/speed to current detected values (prevents wandering)
    void lockPitch(bool lock);
    void lockSpeed(bool lock);
    void setPitchRange(int minHz, int maxHz);

    // Force pitch + speed to specific values and lock both — used by the
    // TX-side decoder (#2417) where the operator's keying parameters are
    // known from PhoneCwApplet rather than detected from the audio.
    // Calling this while a decode is in progress reconfigures ggmorse
    // immediately; subsequent calls are no-ops if the values are unchanged.
    void setKnownParameters(float pitchHz, float speedWpm);
    bool isPitchLocked() const { return m_pitchLocked; }
    bool isSpeedLocked() const { return m_speedLocked; }

public slots:
    // Feed 24kHz stereo int16 PCM (same format as AudioEngine receives).
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void textDecoded(const QString& text, float cost);
    void statsUpdated(float pitchHz, float speedWpm);

private:
    void decodeLoop();

    QThread*      m_workerThread{nullptr};
    std::unique_ptr<GGMorse> m_ggmorse;

    // Ring buffer for audio samples (mono int16 at 24kHz)
    QMutex        m_bufMutex;
    QByteArray    m_ringBuf;
    static constexpr int RING_CAPACITY = 24000 * 2 * 4; // 4 seconds of mono int16

    std::atomic<bool> m_running{false};
    std::atomic<float> m_pitch{0};
    std::atomic<float> m_speed{0};
    std::atomic<bool> m_pitchLocked{false};
    std::atomic<bool> m_speedLocked{false};
    std::atomic<float> m_pitchRangeMin{500.0f};
    std::atomic<float> m_pitchRangeMax{700.0f};
};

} // namespace MasterSDR
