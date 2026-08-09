#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// The two decisions AsrAudioTap has to make about the RX audio it forwards to
// the ASR engine: WHICH receiver's blocks to follow, and how to turn a post-DSP
// stereo block into the mono float32 the engine expects.
//
// Split out of AsrAudioTap because that class is a QObject wired to a concrete
// AudioEngine, and AudioEngine cannot be constructed in a headless test (it
// needs a live QAudioSink). This is Qt-object-free and header-only so the logic
// that can actually be wrong is unit-testable on its own; what is left in the
// tap is a connect() and a call into here.
//
// ── Why a source lock exists at all ───────────────────────────────────────
//
// receivePresentationPostDspAudioReady is emitted from writeAudio(), which runs
// once per RX source: the Flex, the applet Kiwi, and every external Kiwi
// antenna. A consumer that takes all of them receives two or more receivers'
// audio interleaved into one stream. For a waveform that is cosmetic; for a
// speech recogniser it is noise. Copy Assist has no receiver selector of its
// own (it follows "the RX audio"), so the tap picks one and stays with it.
class AsrTapPolicy {
public:
    // A source that has stopped producing blocks for this long has released its
    // claim. Post-DSP audio flows continuously while a receiver is up — a quiet
    // band still produces blocks of near-silence — so a gap this long means the
    // source went away, not that nobody is talking.
    static constexpr qint64 kSourceReleaseMs = 2000;

    // True when this block belongs to the receiver Copy Assist is following.
    //
    // The first block after a reset claims the lock, so a Flex-only station
    // locks Flex and a Kiwi-only station locks Kiwi with no configuration. With
    // both running the choice between them is arbitrary, but it is CONSISTENT,
    // which is the property that matters — an arbitrary single receiver is
    // transcribable and two interleaved ones are not.
    //
    // `nowMs` is passed in rather than read from a clock so the release window
    // is testable without sleeping.
    bool accepts(const QString& source, const QString& sourceId, qint64 nowMs)
    {
        if (m_locked && (source != m_source || sourceId != m_sourceId)) {
            if (nowMs - m_lastAcceptMs < kSourceReleaseMs) {
                return false;
            }
            // The locked source went silent and another is still running:
            // hand the lock over rather than transcribing nothing. This is how
            // an operator switching from the Flex to a Kiwi mid-session is
            // picked up without touching Copy Assist.
            m_locked = false;
        }
        if (!m_locked) {
            m_locked = true;
            m_source = source;
            m_sourceId = sourceId;
        }
        m_lastAcceptMs = nowMs;
        return true;
    }

    // Drop the claim. Called when the tap is disabled, so the next session is
    // free to lock whichever receiver is live then.
    void reset()
    {
        m_locked = false;
        m_source.clear();
        m_sourceId.clear();
        m_lastAcceptMs = 0;
    }

    bool hasLock() const { return m_locked; }
    QString lockedSource() const { return m_source; }
    QString lockedSourceId() const { return m_sourceId; }

    // Collapse interleaved float32 audio to mono, matching what
    // AudioEngine::emitRxPostChainScopeFromFloat32Stereo used to hand us —
    // INCLUDING the non-finite guard. AsrEngine does not sanitise its input, and
    // a single NaN reaching the resampler poisons every sample after it, so
    // dropping that clamp when moving off the engine-side tap would trade a
    // known bug for a worse one.
    //
    // `channels` is the caller's actual channel count (1 or 2), not a guess —
    // receivePresentationPostDspAudioReady is documented stereo today, but
    // inferring channel layout from the byte count's parity (#4489) silently
    // mis-decodes the moment that stops being true: a mono block with an even
    // sample count would be averaged pairwise into half as many frames while
    // the caller kept treating it as full-rate audio. A block that isn't a
    // whole number of frames for the given channel count is malformed, not a
    // shape to guess at, so it is rejected rather than reinterpreted.
    static QVector<float> toMono(const QByteArray& pcmFloat32, int channels)
    {
        if (channels != 1 && channels != 2) {
            return {};
        }
        // A byte count that isn't a whole number of floats truncates below —
        // a block malformed at the sample level, not just the frame level. A
        // 9-byte block claimed as stereo would otherwise become 2 floats,
        // pass the frame check below, and silently drop its trailing byte.
        if (pcmFloat32.size() % static_cast<int>(sizeof(float)) != 0) {
            return {};
        }
        const int totalFloats =
            static_cast<int>(pcmFloat32.size() / static_cast<int>(sizeof(float)));
        if (totalFloats <= 0 || totalFloats % channels != 0) {
            return {};
        }
        const int monoSamples = totalFloats / channels;

        QVector<float> mono(monoSamples);
        const auto* src = reinterpret_cast<const float*>(pcmFloat32.constData());
        for (int i = 0; i < monoSamples; ++i) {
            const float v = (channels == 2)
                ? (src[2 * i] + src[2 * i + 1]) * 0.5f
                : src[i];
            mono[i] = std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f);
        }
        return mono;
    }

private:
    bool    m_locked = false;
    QString m_source;
    QString m_sourceId;
    qint64  m_lastAcceptMs = 0;
};

} // namespace AetherSDR
