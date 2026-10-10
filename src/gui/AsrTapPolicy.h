#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// Where in the RX chain Copy Assist listens.
//
// PostDsp is the historical behaviour and the default: the recogniser hears
// what the speaker plays, after client NR and the RX effects chain. PreDsp
// takes each block before any of that, for operators who find that NR's
// artifacts confuse the speech model more than the noise NR removes. Neither
// undoes processing the radio itself applied.
enum class AsrTapPoint {
    PostDsp,
    PreDsp,
};

// Persisted as a name rather than a bool so a third point (after NR but before
// the effects chain, say) is an added value instead of a migration.
inline QString asrTapPointToSetting(AsrTapPoint point)
{
    return point == AsrTapPoint::PreDsp ? QStringLiteral("PreDsp")
                                        : QStringLiteral("PostDsp");
}

// Anything unrecognised — a hand-edited profile, a value from a build that
// knows a point this one does not — reads as PostDsp, which is what every
// build before this setting did. The dialog then shows the box unchecked, so
// what the operator sees still matches what is being transcribed.
inline AsrTapPoint asrTapPointFromSetting(const QString& value)
{
    return value == QLatin1String("PreDsp") ? AsrTapPoint::PreDsp
                                            : AsrTapPoint::PostDsp;
}

// Small-signal gain of the engine's soft boost, tanh(2x) ~= 2x for small x
// (AudioEngine's RX boost stage). The segmenter's gate decides whether QUIET
// audio is speech, which is exactly that regime, so the linear factor is the
// right one here — the compression tanh applies to loud passages is not
// replicated.
inline constexpr float kRxBoostSmallSignalGain = 2.0f;

// Keep a saved Sensitivity meaning the same thing on both sides of the tap-point
// toggle (RFC #4861, ten9876's "Sensitivity must stay calibrated across the
// toggle").
//
// AsrSegmenter's default gate is an ABSOLUTE RMS threshold driven by the
// Sensitivity slider. The post-DSP feed carries gain the pre-DSP feed does not,
// so it arrives quieter by that amount, and an operator who tuned Sensitivity
// against their post-DSP level would have speech fall under the gate the moment
// they switched to the unprocessed feed. Measured on the real engine with the
// chain at defaults: +0.00 dB, +6.01 dB with boost on, +18.01 dB with boost and
// +12 dB trim.
//
// `rxStaticChainMakeupDb` covers the rest of that gain: AudioEngine sums the RX
// chain stages whose contribution is one number no matter what the audio does
// (EQ master gain, compressor makeup, tube output gain). What NO scalar can
// describe, and what this therefore does not correct for, is the
// signal-dependent part of the chain — EQ band shaping, the gate, compressor
// gain reduction, tube drive and pudu — along with each NR mode's own speech
// attenuation, which already leaves Sensitivity unanchored across NR modes
// (pre-existing, out of scope).
//
// One scalar is left out on purpose: the output pan. It runs before the post-DSP
// emit, and because toMono() averages L+R a hard-panned source reads ~6 dB
// quieter there, so a PreDsp threshold is ~6 dB lower than ideal. It is excluded
// because it applies ONLY to external Kiwi sources (applyKiwiOutputPan), its per-
// source value lives behind an m_dspMutex-guarded lookup, and this function is
// called per audio block — taking the DSP lock at that rate to chase it would put
// GUI-thread contention on the audio path. The error direction is permissive:
// a lower threshold admits more, so no speech is lost to it.
//
// So scale the THRESHOLD rather than the audio: the unprocessed feed stays
// bit-exact (which is the point of the feature, and what the WER evidence was
// gathered on), nothing can clip, and the adjustment is one observable number.
// Returns the rms the segmenter should use for `point`.
//
// NOT a fix for discrimination: on a noisy band the unprocessed feed has little
// level difference between speech and the noise floor, so no threshold
// separates them. Silero VAD is the answer there, and the checkbox's tooltip
// says so.
inline float asrSpeechRmsForTapPoint(float baseRms, AsrTapPoint point,
                                     bool rxBoostOn, float rxOutputTrimDb,
                                     float rxStaticChainMakeupDb = 0.0f)
{
    if (point == AsrTapPoint::PostDsp) {
        return baseRms;   // the level the operator tuned against
    }
    const float boost = rxBoostOn ? kRxBoostSmallSignalGain : 1.0f;
    const float trim = std::pow(10.0f, rxOutputTrimDb / 20.0f);
    const float chain = std::pow(10.0f, rxStaticChainMakeupDb / 20.0f);
    const float makeup = boost * trim * chain;
    // A non-finite or non-positive gain means a corrupt trim value, or an EQ
    // master gain of zero (-inf dB) that silences the post-DSP feed entirely.
    // Neither leaves a threshold worth deriving; the tuned value is the safe
    // reading.
    if (!std::isfinite(makeup) || makeup <= 0.0f) {
        return baseRms;
    }
    return baseRms / makeup;
}

// AsrAudioTap's two decisions: which receiver's blocks to follow, and how to
// turn a block — post-DSP or pre-DSP, per AsrTapPoint — into mono float32.
// Qt-object-free and header-only so it is testable without an AudioEngine
// (which needs a live QAudioSink).
// Source lock: both presentation signals fire once per RX source (Flex, applet
// Kiwi, each external Kiwi), and interleaved receivers are noise to a
// recogniser; Copy Assist has no selector, so the tap picks one and stays.
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
