#pragma once
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <vector>

namespace AetherSDR {

// Thread-safe ring buffer for PCM bytes.
// Written by the demodulator thread, read by the push timer on the main thread.
class WfmRingBuffer
{
public:
    // 1 second at 48 kHz stereo Int16 = 192 000 bytes
    static constexpr int kCapacity = 192000;

    WfmRingBuffer() : m_buf(kCapacity, 0) {}

    void write(const char* data, int len);
    int  read(char* out, int maxLen);
    int  available() const;

private:
    std::vector<char> m_buf;
    int    m_readPos{0};
    int    m_writePos{0};
    int    m_available{0};
    mutable QMutex m_mutex;
};

// Cross-platform audio output backed by QAudioSink in push mode, driven by
// a 10 ms QTimer.  The timer calls bytesFree() and pushes exactly that many
// bytes from the ring buffer each tick — so the audio driver always controls
// the effective data rate.
//
// Works on Windows (WASAPI), macOS (CoreAudio), Linux (PipeWire/PulseAudio).
class WaveOutWriter : public QObject
{
    Q_OBJECT
public:
    explicit WaveOutWriter(QObject* parent = nullptr);
    ~WaveOutWriter() override;

    bool open(const QString& deviceId, int sampleRate, int channelCount = 2);
    void close();

    // Write Int16 interleaved PCM into the ring buffer.  Thread-safe.
    void write(const QByteArray& pcm);

    bool isOpen() const { return m_sink != nullptr; }
    QString deviceName() const { return m_deviceName; }

private slots:
    void pushData();

private:
    WfmRingBuffer m_ring;
    QAudioSink*   m_sink{nullptr};
    QIODevice*    m_io{nullptr};
    QTimer*       m_timer{nullptr};
    QString       m_deviceName;
};

} // namespace AetherSDR
