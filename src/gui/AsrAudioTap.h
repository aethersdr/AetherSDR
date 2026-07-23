#pragma once

#include <QObject>
#include <QMetaObject>

class QByteArray;

namespace AetherSDR {

class AudioEngine;
class AsrEngine;

// App-layer bridge from the engine's post-NR RX audio to the ASR engine
// (RFC #4333, Phase 4). This is the "audio tap": it composes aethercore's
// AudioEngine with aetherasr's AsrEngine without either knowing about the other,
// keeping both libraries decoupled (the whole point of the aetherasr split).
//
// When enabled it forwards the post-client-NR mono RX audio (AudioEngine's
// rxPostChainScopeReady, 24 kHz) to AsrEngine, which resamples to 16 kHz on its
// own worker thread — so nothing here runs on the audio callback. When disabled
// it disconnects entirely, so an idle ASR feature costs nothing.
//
// In the aetherd future this glue moves to the engine/daemon side; the thin UI
// then subscribes to AsrEngine::finalText streamed over the wire and never sees
// audio or whisper.
class AsrAudioTap : public QObject {
    Q_OBJECT
public:
    AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

private:
    void onRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate);

    AudioEngine* m_audio = nullptr;
    AsrEngine* m_asr = nullptr;
    QMetaObject::Connection m_conn;
    bool m_enabled = false;
};

} // namespace AetherSDR
