#include "RNNoiseFilter.h"
#include "Resampler.h"
#include "rnnoise.h"

#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace AetherSDR {

namespace {
Q_LOGGING_CATEGORY(lcRn2, "aether.rn2")
}

// RNNoise frame size: 480 samples at 48kHz = 10ms
static constexpr int FRAME_SIZE = 480;

RNNoiseFilter::RNNoiseFilter(OutputMode outputMode, RateDomain rateDomain)
    : m_outputMode(outputMode)
    , m_rateDomain(rateDomain)
{
    for (int channel = 0; channel < processingChannels(); ++channel) {
        m_states[channel] = rnnoise_create(nullptr);
        if (m_rateDomain == RateDomain::Legacy24k) {
            m_up[channel] = std::make_unique<Resampler>(24000, 48000);
            m_down[channel] = std::make_unique<Resampler>(48000, 24000);
        }
    }
}

RNNoiseFilter::~RNNoiseFilter()
{
    for (DenoiseState* state : m_states) {
        if (state)
            rnnoise_destroy(state);
    }
}

int RNNoiseFilter::processingChannels() const
{
    return m_outputMode == OutputMode::PreserveRxStereo ? 2 : 1;
}

bool RNNoiseFilter::isValid() const
{
    return m_states[0]
        && (m_outputMode == OutputMode::ProcessedMono || m_states[1]);
}

void RNNoiseFilter::setDryMix(float dryMix)
{
    m_dryMix = std::isfinite(dryMix) ? std::clamp(dryMix, 0.0f, 1.0f) : 0.0f;
}

void RNNoiseFilter::reset()
{
    const int channels = processingChannels();
    for (int channel = 0; channel < 2; ++channel) {
        if (m_states[channel])
            rnnoise_destroy(m_states[channel]);
        const bool inUse = channel < channels;
        m_states[channel] = inUse ? rnnoise_create(nullptr) : nullptr;
        const bool needsResamplers = inUse
            && m_rateDomain == RateDomain::Legacy24k;
        m_up[channel] = needsResamplers
            ? std::make_unique<Resampler>(24000, 48000)
            : nullptr;
        m_down[channel] = needsResamplers
            ? std::make_unique<Resampler>(48000, 24000)
            : nullptr;
        m_inAccum[channel].resize(0);
        m_input24k[channel].clear();
        m_processed48k[channel].clear();
        m_processed48kFloat[channel].clear();
    }
    m_outAccum.resize(0);
}

QByteArray RNNoiseFilter::process(const QByteArray& pcm24kStereo)
{
    if (m_rateDomain != RateDomain::Legacy24k) {
        qCWarning(lcRn2)
            << "RNNoiseFilter: 24 kHz process() called on native-48 kHz instance";
        return pcm24kStereo;
    }
    if (!isValid() || pcm24kStereo.isEmpty())
        return pcm24kStereo;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoFrames =
        pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    const int channels = processingChannels();

    // 1. Split RX stereo into matched channel paths. ProcessedMono is the TX
    //    path and intentionally retains the historical L/R downmix.
    for (int channel = 0; channel < channels; ++channel)
        m_input24k[channel].resize(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i) {
        if (channels == 2) {
            m_input24k[0][i] = src[i * 2];
            m_input24k[1][i] = src[i * 2 + 1];
        } else {
            m_input24k[0][i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
        }
    }

    // 2. Append to input accumulator and process complete 480-sample frames
    //    RNNoise expects [-32768, 32768] range, so scale from [-1, 1]
    std::array<int, 2> totalAccumSamples{0, 0};
    int completeFrames = std::numeric_limits<int>::max();
    for (int channel = 0; channel < channels; ++channel) {
        const QByteArray mono48k =
            m_up[channel]->process(m_input24k[channel].data(), stereoFrames);
        const auto* mono48kSamples =
            reinterpret_cast<const float*>(mono48k.constData());
        const int monoSamples48k = mono48k.size() / static_cast<int>(sizeof(float));
        const int startIdx =
            m_inAccum[channel].size() / static_cast<int>(sizeof(float));
        m_inAccum[channel].resize((startIdx + monoSamples48k)
                                  * static_cast<int>(sizeof(float)));
        auto* floatBuf = reinterpret_cast<float*>(m_inAccum[channel].data());
        for (int i = 0; i < monoSamples48k; ++i)
            floatBuf[startIdx + i] = mono48kSamples[i] * 32768.0f;
        totalAccumSamples[channel] = startIdx + monoSamples48k;
        completeFrames =
            std::min(completeFrames, totalAccumSamples[channel] / FRAME_SIZE);
    }

    if (completeFrames > 0) {
        const int consumedSamples = completeFrames * FRAME_SIZE;
        const int outputMonoSamples = consumedSamples;
        std::array<QByteArray, 2> downsampled;

        // Both channels run the same frame count off the same block, so their
        // RNNoise states advance in lockstep — that lockstep IS the fix, and
        // step 4 below asserts it survived the resamplers.
        for (int channel = 0; channel < channels; ++channel) {
            m_processed48k[channel].resize(outputMonoSamples);
            auto* accumData = reinterpret_cast<float*>(m_inAccum[channel].data());
            for (int f = 0; f < completeFrames; ++f) {
                if (m_dryMix > 0.0f) {
                    rnnoise_process_frame_with_dry_mix(
                        m_states[channel],
                        &m_processed48k[channel][f * FRAME_SIZE],
                        &accumData[f * FRAME_SIZE],
                        m_dryMix);
                } else {
                    rnnoise_process_frame(m_states[channel],
                                          &m_processed48k[channel][f * FRAME_SIZE],
                                          &accumData[f * FRAME_SIZE]);
                }
            }

            // Keep leftover input samples
            const int leftoverSamples = totalAccumSamples[channel] - consumedSamples;
            if (leftoverSamples > 0) {
                m_inAccum[channel] = QByteArray(
                    reinterpret_cast<const char*>(&accumData[consumedSamples]),
                    leftoverSamples * static_cast<int>(sizeof(float)));
            } else {
                m_inAccum[channel].resize(0);
            }

            // 3. Scale RNNoise output back to [-1, 1], then downsample each
            //    channel through its matched 48kHz → 24kHz path.
            m_processed48kFloat[channel].resize(outputMonoSamples);
            for (int i = 0; i < outputMonoSamples; ++i)
                m_processed48kFloat[channel][i] = m_processed48k[channel][i] / 32768.0f;
            downsampled[channel] = m_down[channel]->process(
                m_processed48kFloat[channel].data(), outputMonoSamples);
        }

        // 4. Interleave. The two resamplers are identically configured and fed
        //    identical sample counts, so they cannot return different lengths.
        //    If that ever changes, truncating to the shorter one desyncs L/R
        //    permanently and *silently* — the exact failure this filter exists
        //    to prevent — so say so once instead of drifting quietly.
        int downsampledFrames =
            downsampled[0].size() / static_cast<int>(sizeof(float));
        if (channels == 2) {
            const int rightFrames =
                downsampled[1].size() / static_cast<int>(sizeof(float));
            if (rightFrames != downsampledFrames) {
                if (!m_warnedChannelLengthMismatch) {
                    m_warnedChannelLengthMismatch = true;
                    qCWarning(lcRn2)
                        << "RN2 channel resamplers diverged (L" << downsampledFrames
                        << "frames, R" << rightFrames
                        << ") — stereo image will drift; RN2 output is unreliable";
                }
                downsampledFrames = std::min(downsampledFrames, rightFrames);
            }
        }

        QByteArray processedStereo(
            downsampledFrames * 2 * static_cast<int>(sizeof(float)),
            Qt::Uninitialized);
        auto* stereo = reinterpret_cast<float*>(processedStereo.data());
        const auto* left =
            reinterpret_cast<const float*>(downsampled[0].constData());
        const auto* right =
            channels == 2
                ? reinterpret_cast<const float*>(downsampled[1].constData())
                : left;
        for (int i = 0; i < downsampledFrames; ++i) {
            stereo[i * 2] = left[i];
            stereo[i * 2 + 1] = right[i];
        }

        m_outAccum.append(processedStereo);
    }

    // 5. Return exactly the same number of bytes as input
    const int needed = pcm24kStereo.size();
    if (m_outAccum.size() >= needed) {
        QByteArray result = m_outAccum.left(needed);
        m_outAccum.remove(0, needed);
        return result;
    }

    // Not enough output yet — return silence (only happens during startup)
    return QByteArray(needed, '\0');
}

int RNNoiseFilter::process48kStereo(
    const QByteArray& pcm48kStereo, QByteArray& output)
{
    output.resize(0);
    if (m_rateDomain != RateDomain::Native48k) {
        qCWarning(lcRn2)
            << "RNNoiseFilter: process48kStereo() requires native-48 kHz rate domain";
        output.resize(pcm48kStereo.size());
        if (!pcm48kStereo.isEmpty()) {
            std::memcpy(output.data(), pcm48kStereo.constData(),
                        pcm48kStereo.size());
        }
        return output.size() / (2 * static_cast<int>(sizeof(float)));
    }
    if (!isValid() || pcm48kStereo.isEmpty()) {
        output.resize(pcm48kStereo.size());
        if (!pcm48kStereo.isEmpty()) {
            std::memcpy(output.data(), pcm48kStereo.constData(),
                        pcm48kStereo.size());
        }
        return output.size() / (2 * static_cast<int>(sizeof(float)));
    }

    const int stereoFrames =
        pcm48kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    if (stereoFrames <= 0) {
        return 0;
    }

    const auto* src = reinterpret_cast<const float*>(pcm48kStereo.constData());
    const int channels = processingChannels();
    int completeFrames = std::numeric_limits<int>::max();
    std::array<int, 2> totalAccumSamples{0, 0};

    for (int channel = 0; channel < channels; ++channel) {
        const int start = m_inAccum[channel].size() / static_cast<int>(sizeof(float));
        m_inAccum[channel].resize(
            (start + stereoFrames) * static_cast<int>(sizeof(float)));
        auto* accum = reinterpret_cast<float*>(m_inAccum[channel].data());
        for (int frame = 0; frame < stereoFrames; ++frame) {
            const float sample = channels == 2
                ? src[frame * 2 + channel]
                : 0.5f * (src[frame * 2] + src[frame * 2 + 1]);
            accum[start + frame] = sample * 32768.0f;
        }
        totalAccumSamples[channel] = start + stereoFrames;
        completeFrames = std::min(
            completeFrames, totalAccumSamples[channel] / FRAME_SIZE);
    }

    if (completeFrames > 0) {
        const int consumedSamples = completeFrames * FRAME_SIZE;
        for (int channel = 0; channel < channels; ++channel) {
            m_processed48k[channel].resize(consumedSamples);
            auto* accum = reinterpret_cast<float*>(m_inAccum[channel].data());
            for (int frame = 0; frame < completeFrames; ++frame) {
                float* frameOutput =
                    &m_processed48k[channel][frame * FRAME_SIZE];
                float* input = &accum[frame * FRAME_SIZE];
                if (m_dryMix > 0.0f) {
                    rnnoise_process_frame_with_dry_mix(
                        m_states[channel], frameOutput, input, m_dryMix);
                } else {
                    rnnoise_process_frame(m_states[channel], frameOutput, input);
                }
            }

            const int leftover = totalAccumSamples[channel] - consumedSamples;
            if (leftover > 0) {
                std::memmove(accum, &accum[consumedSamples],
                             leftover * sizeof(float));
                m_inAccum[channel].resize(
                    leftover * static_cast<int>(sizeof(float)));
            } else {
                m_inAccum[channel].resize(0);
            }
        }

        const int oldOutputSamples =
            m_outAccum.size() / static_cast<int>(sizeof(float));
        m_outAccum.resize(
            (oldOutputSamples + consumedSamples * 2)
            * static_cast<int>(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(m_outAccum.data())
            + oldOutputSamples;
        for (int frame = 0; frame < consumedSamples; ++frame) {
            const float left = m_processed48k[0][frame] / 32768.0f;
            const float right = channels == 2
                ? m_processed48k[1][frame] / 32768.0f
                : left;
            dst[frame * 2] = left;
            dst[frame * 2 + 1] = right;
        }
    }

    const int needed = pcm48kStereo.size();
    if (m_outAccum.size() >= needed) {
        output.resize(needed);
        std::memcpy(output.data(), m_outAccum.constData(), needed);
        m_outAccum.remove(0, needed);
        return stereoFrames;
    }
    output.fill('\0', needed);
    return stereoFrames;
}

} // namespace AetherSDR
