#pragma once

#include "gui/AsrTapPolicy.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>

class QByteArray;

namespace AetherSDR {

class AudioEngine;
class AsrEngine;

// App-layer bridge from post-NR RX audio to the ASR engine (RFC #4333),
// composing AudioEngine and AsrEngine without either knowing the other. When
// enabled it forwards post-DSP mono RX audio, which AsrEngine resamples to
// 16 kHz on its own worker (nothing runs on the audio callback); disabled, it
// disconnects entirely.
// Subscribes to receivePresentationPostDspAudioReady, NOT rxPostChainScopeReady
// (#4486): the scope signal drops blocks within 8 ms of the previous one, and
// with NR2 blocks arrive in tight bursts, so it would discard ~half the speech.
// The presentation signal is unthrottled and tagged with its source, letting
// the tap follow one receiver (see AsrTapPolicy). Don't relax the scope
// throttle; its consumers (StripWaveformPanel, MainWindow) want it.
class AsrAudioTap : public QObject {
    Q_OBJECT
public:
    AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

private:
    void onRxAudio(const QString& source,
                   const QString& sourceId,
                   const QByteArray& pcmFloat,
                   int sampleRate,
                   int channels);

    AudioEngine* m_audio = nullptr;
    AsrEngine* m_asr = nullptr;
    QMetaObject::Connection m_conn;
    bool m_enabled = false;
    AsrTapPolicy m_policy;
    QElapsedTimer m_clock;   // monotonic source for the policy's release window
    // Latch so a block toMono() cannot decode warns once per enable rather
    // than once per audio block. Cleared in setEnabled(true).
    bool m_warnedUndecodable = false;
};

} // namespace AetherSDR
