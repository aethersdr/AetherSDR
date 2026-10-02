#include "AsrAudioTap.h"

#include "asr/AsrEngine.h"
#include "core/AudioEngine.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QVector>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrTap, "aether.asr.tap")

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
        m_warnedUndecodable = false;
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
                            const QByteArray& pcmFloat,
                            int sampleRate,
                            int channels)
{
    if (!m_enabled || m_asr == nullptr) {
        return;
    }
    if (!m_policy.accepts(source, sourceId,
                          m_clock.isValid() ? m_clock.elapsed() : 0)) {
        return;
    }
    // channels comes straight off the signal (#4489) instead of being
    // assumed here — a future mono RX source is then a one-line change at
    // AudioEngine's emit site, and toMono() rejects a caller that gets it
    // wrong (and says so below) instead of silently mis-decoding.
    const QVector<float> mono = AsrTapPolicy::toMono(pcmFloat, channels);
    if (mono.isEmpty()) {
        // Warn about an undecodable block: otherwise Copy Assist silently produces
        // nothing, which looks like a model or wiring failure. The guard targets an
        // emit site stating the wrong channel count, which is permanent, so warn once
        // per enable (setEnabled() clears the latch), not per ~5 ms block.
        if (!m_warnedUndecodable) {
            m_warnedUndecodable = true;
            qCWarning(lcAsrTap) << "dropping undecodable RX audio from" << source
                                << "id" << sourceId << "- bytes" << pcmFloat.size()
                                << "channels" << channels
                                << "(expected 1 or 2, a whole number of frames)";
        }
        return;
    }
    m_asr->pushAudio(mono, sampleRate);
}

} // namespace AetherSDR
