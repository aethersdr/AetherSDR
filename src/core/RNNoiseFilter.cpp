#include "RNNoiseFilter.h"
#include "Resampler.h"
#include "rnnoise.h"

#include <cstring>
#include <vector>

namespace MasterSDR {

// RNNoise frame size: 480 samples at 48kHz = 10ms
static constexpr int FRAME_SIZE = 480;

RNNoiseFilter::RNNoiseFilter()
    : m_state(rnnoise_create(nullptr))
    , m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
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
}

QByteArray RNNoiseFilter::process(const QByteArray& pcm24kStereo)
{
    if (!m_state || pcm24kStereo.isEmpty())
        return pcm24kStereo;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoFrames = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // 1. Upsample 24kHz stereo float32 → 48kHz mono float32 via r8brain
    QByteArray mono48k = m_up->processStereoToMono(src, stereoFrames);

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
        std::vector<float> processed48k(completeFrames * FRAME_SIZE);

        for (int f = 0; f < completeFrames; ++f) {
            rnnoise_process_frame(m_state,
                                  &processed48k[f * FRAME_SIZE],
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
        std::vector<float> processed48kFloat(outputMonoSamples);
        for (int i = 0; i < outputMonoSamples; ++i)
            processed48kFloat[i] = processed48k[i] / 32768.0f;

        // Downsample 48kHz mono → 24kHz stereo via r8brain
        QByteArray downsampled = m_down->processMonoToStereo(
            processed48kFloat.data(), outputMonoSamples);

        m_outAccum.append(downsampled);
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

} // namespace MasterSDR
