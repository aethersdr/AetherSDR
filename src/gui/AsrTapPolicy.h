#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// AsrAudioTap's two decisions: which receiver's blocks to follow, and how to
// turn a post-DSP block into mono float32. Qt-object-free and header-only so it
// is testable without an AudioEngine (which needs a live QAudioSink).
// Source lock: receivePresentationPostDspAudioReady fires once per RX source
// (Flex, applet Kiwi, each external Kiwi), and interleaved receivers are noise
// to a recogniser; Copy Assist has no selector, so the tap picks one and stays.
class AsrTapPolicy {
public:
    // A source that has stopped producing blocks for this long has released its
    // claim. Post-DSP audio flows continuously while a receiver is up — a quiet
    // band still produces blocks of near-silence — so a gap this long means the
    // source went away, not that nobody is talking.
    static constexpr qint64 kSourceReleaseMs = 2000;

    // True when this block belongs to the followed receiver. The first block after
    // a reset claims the lock: arbitrary when several run, but consistent. `nowMs`
    // is injected so the release window is testable without sleeping.
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

    // Collapse interleaved float32 to mono, including the non-finite guard:
    // AsrEngine doesn't sanitise, and one NaN poisons its resampler. `channels` is
    // the caller's actual count (1 or 2), never inferred from byte-count parity
    // (#4489); a block that isn't a whole number of frames is rejected.
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
