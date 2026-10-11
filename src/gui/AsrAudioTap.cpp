#include "AsrAudioTap.h"

#include "asr/AsrEngine.h"
#include "core/AudioEngine.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QVector>

#include <cmath>

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
        connectSelectedTap();
    } else {
        disconnect(m_conn);
        m_policy.reset();
        // m_asr->setEnabled(false) above already resets and drops any queued
        // backlog — no separate reset() needed here.
    }
}

void AsrAudioTap::setTapPoint(AsrTapPoint point)
{
    if (point == m_tapPoint) {
        return;
    }
    m_tapPoint = point;
    if (!m_enabled) {
        return; // the next setEnabled(true) connects the new point
    }

    disconnect(m_conn);
    // Start over rather than carry on. An utterance begun through one chain
    // and finished through the other is spliced across a level step and a
    // latency step (an NR stage delays its output), and the receiver lock was
    // earned on the other signal. restartSession() drops the partial
    // utterance, carried context and speaker clusters, AND the audio already
    // queued to the worker — all of which came from the other chain. A bare
    // reset() would keep worker ordering correct but still let the old chain's
    // backlog be transcribed, so with a long decode buffer the operator would
    // read old-chain text well after the switch. Speaker clusters go either
    // way: an embedding shifts with NR, so they would not survive intact.
    m_policy.reset();
    m_clock.restart();
    m_warnedUndecodable = false;
    if (m_asr != nullptr) {
        m_asr->restartSession();
    }
    // The two points arrive at different levels, so the gate's threshold has
    // to move with the switch or a saved Sensitivity silently means something
    // else on the other side.
    m_speechRmsApplied = false;
    applySpeechRmsForTapPoint();
    connectSelectedTap();
}

void AsrAudioTap::setBaseSpeechRms(float rms)
{
    m_baseSpeechRms = rms;
    m_speechRmsApplied = false;   // force a push even if the gain is unchanged
    applySpeechRmsForTapPoint();
}

void AsrAudioTap::applySpeechRmsForTapPoint()
{
    if (m_asr == nullptr || m_audio == nullptr || m_baseSpeechRms <= 0.0f) {
        return;   // nothing seeded yet
    }
    const bool boost = m_audio->rxBoost();
    const float trimDb = m_audio->rxOutputTrimDb();
    const float chainDb = m_audio->rxStaticChainMakeupDb();
    const float rms =
        asrSpeechRmsForTapPoint(m_baseSpeechRms, m_tapPoint, boost, trimDb, chainDb);
    // Change detection on the gain, not the rms, so float noise in the trim
    // does not re-push every block.
    const float gainDb = (m_tapPoint == AsrTapPoint::PostDsp)
                             ? 0.0f
                             : (boost ? 6.0206f : 0.0f) + trimDb + chainDb;
    // A non-finite total (an EQ master gain of zero is -inf dB) means the
    // post-DSP feed is silenced, and the scaler above already fell back to the
    // tuned value — the same rms 0 dB would give. Track it as 0 dB so the
    // comparison stays finite; comparing infinities yields NaN, which is never
    // < 0.01 and would re-push on every block.
    const float trackedGainDb = std::isfinite(gainDb) ? gainDb : 0.0f;
    if (m_speechRmsApplied && std::fabs(trackedGainDb - m_appliedGainDb) < 0.01f) {
        return;
    }
    m_appliedGainDb = trackedGainDb;
    m_speechRmsApplied = true;
    m_asr->setSpeechRms(rms);
}

void AsrAudioTap::connectSelectedTap()
{
    if (m_audio == nullptr) {
        return;
    }
    // Queued so the audio-thread emit lands on this (main) thread; the heavy
    // resample+inference then happens on the ASR worker thread.
    //
    // Both signals are unthrottled by design — see the note in the header.
    // Every block they carry must reach the engine.
    if (m_tapPoint == AsrTapPoint::PreDsp) {
        m_conn = connect(m_audio, &AudioEngine::receivePresentationPreDspAudioReady,
                         this, &AsrAudioTap::onPreDspAudio, Qt::QueuedConnection);
    } else {
        m_conn = connect(m_audio, &AudioEngine::receivePresentationPostDspAudioReady,
                         this, &AsrAudioTap::onPostDspAudio, Qt::QueuedConnection);
    }
}

void AsrAudioTap::onPostDspAudio(const QString& source, const QString& sourceId,
                                 const QByteArray& pcmFloat, int sampleRate,
                                 int channels)
{
    onRxAudio(AsrTapPoint::PostDsp, source, sourceId, pcmFloat, sampleRate, channels);
}

void AsrAudioTap::onPreDspAudio(const QString& source, const QString& sourceId,
                                const QByteArray& pcmFloat, int sampleRate,
                                int channels)
{
    onRxAudio(AsrTapPoint::PreDsp, source, sourceId, pcmFloat, sampleRate, channels);
}

void AsrAudioTap::onRxAudio(AsrTapPoint from,
                            const QString& source,
                            const QString& sourceId,
                            const QByteArray& pcmFloat,
                            int sampleRate,
                            int channels)
{
    // disconnect() does not withdraw calls a queued connection has already
    // posted, so blocks from the old point can still arrive after a switch —
    // after the engine reset above, where they would open the new session
    // with audio from the wrong chain. They name their point; drop them.
    if (!m_enabled || m_asr == nullptr || from != m_tapPoint) {
        return;
    }

    // AudioEngine publishes no change signal for the RX boost or the output
    // trim, so the gain is re-read here. Cheap: two atomic loads and a
    // compare, and setSpeechRms is only called when the gain actually moves.
    applySpeechRmsForTapPoint();
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
