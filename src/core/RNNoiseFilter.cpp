#include "RNNoiseFilter.h"
#include "Resampler.h"
#include "rnnoise.h"

#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {
Q_LOGGING_CATEGORY(lcRn2, "aether.rn2")
}

// RNNoise frame size: 480 samples at 48kHz = 10ms
static constexpr int FRAME_SIZE = 480;

RNNoiseFilter::RNNoiseFilter(OutputMode outputMode)
    : m_outputMode(outputMode)
{
    for (int channel = 0; channel < processingChannels(); ++channel) {
        m_states[channel] = rnnoise_create(nullptr);
        m_up[channel] = std::make_unique<Resampler>(24000, 48000);
        m_down[channel] = std::make_unique<Resampler>(48000, 24000);
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
        m_up[channel] = inUse ? std::make_unique<Resampler>(24000, 48000) : nullptr;
        m_down[channel] = inUse ? std::make_unique<Resampler>(48000, 24000) : nullptr;
        m_inAccum[channel].clear();
        m_input24k[channel].clear();
        m_processed48k[channel].clear();
        m_processed48kFloat[channel].clear();
    }
    m_outAccum.clear();
}

QByteArray RNNoiseFilter::process(const QByteArray& pcm24kStereo)
{
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
                m_inAccum[channel].clear();
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

} // namespace AetherSDR
