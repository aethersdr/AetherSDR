#include "AsrAudioTap.h"

#include "asr/AsrEngine.h"
#include "core/AudioEngine.h"

#include <QByteArray>
#include <QVector>

namespace AetherSDR {

AsrAudioTap::AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent)
    : QObject(parent)
    , m_audio(audio)
    , m_asr(asr)
{
}

void AsrAudioTap::setEnabled(bool on)
{
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    if (m_asr != nullptr) {
        m_asr->setEnabled(on);
    }

    if (on) {
        // A fresh session picks its receiver again — the one that was live last
        // time may be gone, and the operator would otherwise have to wait out
        // the release window before anything was transcribed.
        m_policy.reset();
        m_clock.start();
        if (m_audio != nullptr) {
            // Queued so the audio-thread emit lands on this (main) thread; the
            // heavy resample+inference then happens on the ASR worker thread.
            //
            // Unthrottled by design — see the note in the header. Every block
            // this signal carries must reach the engine.
            m_conn = connect(m_audio, &AudioEngine::receivePresentationPostDspAudioReady,
                             this, &AsrAudioTap::onRxAudio, Qt::QueuedConnection);
        }
    } else {
        disconnect(m_conn);
        m_policy.reset();
        // m_asr->setEnabled(false) above already resets and drops any queued
        // backlog — no separate reset() needed here.
    }
}

void AsrAudioTap::onRxAudio(const QString& source,
                            const QString& sourceId,
                            const QByteArray& stereoFloat32Pcm,
                            int sampleRate)
{
    if (!m_enabled || m_asr == nullptr) {
        return;
    }
    if (!m_policy.accepts(source, sourceId,
                          m_clock.isValid() ? m_clock.elapsed() : 0)) {
        return;
    }
    const QVector<float> mono = AsrTapPolicy::toMono(stereoFloat32Pcm);
    if (mono.isEmpty()) {
        return;
    }
    m_asr->pushAudio(mono, sampleRate);
}

} // namespace AetherSDR
