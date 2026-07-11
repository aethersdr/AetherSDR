#include "RNNoiseFilter.h"
#include "Resampler.h"
#include "rnnoise.h"

#include <cstring>
#include <vector>

namespace AetherSDR {

// RNNoise frame size: 480 samples at 48kHz = 10ms
static constexpr int FRAME_SIZE = 480;

RNNoiseFilter::RNNoiseFilter(OutputMode outputMode)
    : m_state(rnnoise_create(nullptr))
    , m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
    , m_outputMode(outputMode)
{}

RNNoiseFilter::~RNNoiseFilter()
{
    if (m_state)
        rnnoise_destroy(m_state);
}

void RNNoiseFilter::reset()
{
    if (m_state)
        rnnoise_destroy(m_state);
    m_state = rnnoise_create(nullptr);
    m_up = std::make_unique<Resampler>(24000, 48000);
    m_down = std::make_unique<Resampler>(48000, 24000);
    m_inAccum.clear();
    m_outAccum.clear();
    m_stereoAdapter.reset();
}

QByteArray RNNoiseFilter::process(const QByteArray& pcm24kStereo)
{
    if (!m_state || pcm24kStereo.isEmpty())
        return pcm24kStereo;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoFrames = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    if (m_outputMode == OutputMode::PreserveRxStereo) {
        m_stereoAdapter.pushDryStereo(pcm24kStereo);
    }

    // 1. Downmix, then upsample 24kHz mono float32 → 48kHz mono float32 via r8brain.
    // The dry stereo stays queued so the RNNoise gain can be applied to both channels.
    m_mono24k.resize(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i) {
        m_mono24k[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
    }
    QByteArray mono48k = m_up->process(m_mono24k.data(), stereoFrames);

    const auto* mono48kSamples = reinterpret_cast<const float*>(mono48k.constData());
    const int monoSamples48k = mono48k.size() / static_cast<int>(sizeof(float));

    // 2. Append to input accumulator and process complete 480-sample frames
    //    RNNoise expects [-32768, 32768] range, so scale from [-1, 1]
    const int prevAccumSamples = m_inAccum.size() / static_cast<int>(sizeof(float));
    {
        const int startIdx = prevAccumSamples;
        m_inAccum.resize((startIdx + monoSamples48k) * sizeof(float));
        auto* floatBuf = reinterpret_cast<float*>(m_inAccum.data());
        for (int i = 0; i < monoSamples48k; ++i)
            floatBuf[startIdx + i] = mono48kSamples[i] * 32768.0f;
    }

    const int totalAccumSamples = prevAccumSamples + monoSamples48k;
    const int completeFrames = totalAccumSamples / FRAME_SIZE;

    if (completeFrames > 0) {
        auto* accumData = reinterpret_cast<float*>(m_inAccum.data());
        m_processed48k.resize(completeFrames * FRAME_SIZE);

        for (int f = 0; f < completeFrames; ++f) {
            rnnoise_process_frame(m_state,
                                  &m_processed48k[f * FRAME_SIZE],
                                  &accumData[f * FRAME_SIZE]);
        }

        // Keep leftover input samples
        const int consumedSamples = completeFrames * FRAME_SIZE;
        const int leftoverSamples = totalAccumSamples - consumedSamples;
        if (leftoverSamples > 0) {
            QByteArray leftover(reinterpret_cast<const char*>(&accumData[consumedSamples]),
                                leftoverSamples * sizeof(float));
            m_inAccum = leftover;
        } else {
            m_inAccum.clear();
        }

        // 3. Scale processed 48kHz float from RNNoise range [-32768,32768] to [-1,1],
        //    then downsample to 24kHz stereo float32
        const int outputMonoSamples = completeFrames * FRAME_SIZE;
        m_processed48kFloat.resize(outputMonoSamples);
        for (int i = 0; i < outputMonoSamples; ++i)
            m_processed48kFloat[i] = m_processed48k[i] / 32768.0f;

        // Downsample 48kHz mono → 24kHz mono, then apply the shared gain envelope
        // to the delayed dry stereo so slice/diversity balance survives RN2.
        QByteArray downsampled = m_down->process(
            m_processed48kFloat.data(), outputMonoSamples);
        const auto* downsampledMono = reinterpret_cast<const float*>(downsampled.constData());
        const int downsampledFrames = downsampled.size() / static_cast<int>(sizeof(float));

        if (m_outputMode == OutputMode::PreserveRxStereo) {
            m_outAccum.append(
                m_stereoAdapter.takeProcessedMono(downsampledMono, downsampledFrames));
        } else {
            QByteArray processedStereo(
                downsampledFrames * 2 * static_cast<int>(sizeof(float)),
                Qt::Uninitialized);
            auto* stereo = reinterpret_cast<float*>(processedStereo.data());
            for (int i = 0; i < downsampledFrames; ++i) {
                stereo[i * 2] = downsampledMono[i];
                stereo[i * 2 + 1] = downsampledMono[i];
            }
            m_outAccum.append(processedStereo);
        }
    }

    // 4. Return exactly the same number of bytes as input
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
