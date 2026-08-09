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
        // Rejecting a block toMono() cannot decode is right, but it is also
        // invisible: Copy Assist simply produces nothing, which looks exactly
        // like a model that failed to load or a tap that never connected. The
        // guard exists to catch a FUTURE emit site stating the wrong channel
        // count, so the one person who needs this message is the one who has
        // no reason to suspect this code at all — say it out loud.
        //
        // Once per enable, not per block: this runs on every audio block
        // (~5 ms apart on a Flex LAN stream), and a mis-stated channel count
        // is permanent for a given emit site, so the first block says
        // everything the log needs. setEnabled() clears the latch so a later
        // session reports again.
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
