#pragma once

#include <QAudio>
#include <QAudioDevice>
#include <QBuffer>
#include <QByteArray>
#include <QObject>
#include <QString>

class QAudioSink;

namespace AetherSDR {

// Plays a WAV file on this computer's speakers — the client-side voice keyer's
// PLAY (RFC #4214). Never touches the radio or the transmitter.
//
// Follows QsoRecorder's playback pattern: the output device the operator chose
// (fed through AudioOutputRouter), the shared format ladder, and a request to
// mute live RX for the length of the preview — emitted only once the sink has
// actually started, so a failed open can never strand RX muted (#3230).
class LocalWavPlayer : public QObject {
    Q_OBJECT
public:
    explicit LocalWavPlayer(QObject* parent = nullptr);
    ~LocalWavPlayer() override;

    void setOutputDevice(const QAudioDevice& dev) { m_outputDevice = dev; }

    // Starts playback; false with a reason when the file or device is unusable.
    bool play(const QString& path, QString& error);
    void stop();
    bool isPlaying() const { return m_playing; }

signals:
    void finished();                  // played to the end, or stopped
    void muteRxRequested(bool mute);

private:
    void onSinkState(QAudio::State state);

    QAudioDevice m_outputDevice;
    QAudioSink*  m_sink{nullptr};
    QBuffer      m_buffer;
    QByteArray   m_pcm;
    bool         m_playing{false};
};

} // namespace AetherSDR
