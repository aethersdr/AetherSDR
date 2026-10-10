#pragma once

#include "gui/AsrTapPolicy.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>

class QByteArray;

namespace AetherSDR {

class AudioEngine;
class AsrEngine;

// App-layer bridge from RX audio to the ASR engine (RFC #4333), composing
// AudioEngine and AsrEngine without either knowing the other. When enabled it
// forwards mono RX audio, which AsrEngine resamples to 16 kHz on its own worker
// (nothing runs on the audio callback); disabled, it disconnects entirely.
// Subscribes to the presentation signals, NOT rxPostChainScopeReady (#4486):
// the scope signal drops blocks within 8 ms of the previous one, and with NR2
// blocks arrive in tight bursts, so it would discard ~half the speech. The
// presentation signals are unthrottled and tagged with their source, letting
// the tap follow one receiver (see AsrTapPolicy). Don't relax the scope
// throttle; its consumers (StripWaveformPanel, MainWindow) want it.
// Tap point: post-DSP by default; the operator can move it to
// receivePresentationPreDspAudioReady, the same unthrottled, source-tagged
// stream taken before client NR and the RX effects, because NR artifacts can
// confuse the model more than the noise NR removes. Each signal has its own
// slot naming its point, so a block queued from the old point before a switch
// is dropped rather than spliced into the new stream.
class AsrAudioTap : public QObject {
    Q_OBJECT
public:
    AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    // Applied live. Switching while enabled starts transcription over —
    // including the audio already queued to the ASR worker, which came from
    // the other chain (see the .cpp); switching while disabled only takes
    // effect at the next enable.
    void setTapPoint(AsrTapPoint point);
    AsrTapPoint tapPoint() const { return m_tapPoint; }

    // The operator's Sensitivity, as an absolute RMS and UNSCALED. The tap
    // owns the scaling because only it knows which point is live, and it
    // re-checks the engine's boost/trim per block — AudioEngine publishes no
    // change signal for either, so a controller-side computation would go
    // stale the moment the operator moved one.
    void setBaseSpeechRms(float rms);

private:
    void connectSelectedTap();
    // Push a rescaled threshold only when the computed gain actually changes.
    void applySpeechRmsForTapPoint();
    void onPostDspAudio(const QString& source, const QString& sourceId,
                        const QByteArray& pcmFloat, int sampleRate, int channels);
    void onPreDspAudio(const QString& source, const QString& sourceId,
                       const QByteArray& pcmFloat, int sampleRate, int channels);
    void onRxAudio(AsrTapPoint from,
                   const QString& source,
                   const QString& sourceId,
                   const QByteArray& pcmFloat,
                   int sampleRate,
                   int channels);

    AudioEngine* m_audio = nullptr;
    AsrEngine* m_asr = nullptr;
    QMetaObject::Connection m_conn;
    bool m_enabled = false;
    AsrTapPoint m_tapPoint = AsrTapPoint::PostDsp;
    AsrTapPolicy m_policy;
    QElapsedTimer m_clock;   // monotonic source for the policy's release window
    // Latch so a block toMono() cannot decode warns once per enable rather
    // than once per audio block. Cleared in setEnabled(true).
    bool m_warnedUndecodable = false;
    float m_baseSpeechRms = 0.0f;   // 0 = the controller has not seeded it yet
    float m_appliedGainDb = 0.0f;   // last gain pushed, as dB, for change detection
    bool m_speechRmsApplied = false;
};

} // namespace AetherSDR
