#include "AsrAudioTap.h"

#include "asr/AsrEngine.h"
#include "core/AudioEngine.h"

#include <QByteArray>
#include <QVector>

#include <algorithm>

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
        if (m_audio != nullptr) {
            // Queued so the audio-thread emit lands on this (main) thread; the
            // heavy resample+inference then happens on the ASR worker thread.
            m_conn = connect(m_audio, &AudioEngine::rxPostChainScopeReady,
                             this, &AsrAudioTap::onRxAudio, Qt::QueuedConnection);
        }
    } else {
        disconnect(m_conn);
        // m_asr->setEnabled(false) above already resets and drops any queued
        // backlog — no separate reset() needed here.
    }
}

void AsrAudioTap::onRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    if (!m_enabled || m_asr == nullptr) {
        return;
    }
    const int n = monoFloat32Pcm.size() / static_cast<int>(sizeof(float));
    if (n <= 0) {
        return;
    }
    const auto* f = reinterpret_cast<const float*>(monoFloat32Pcm.constData());
    QVector<float> mono(n);
    std::copy(f, f + n, mono.begin());
    m_asr->pushAudio(mono, sampleRate);
}

} // namespace AetherSDR
