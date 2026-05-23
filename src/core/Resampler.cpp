#include "Resampler.h"

#include "CDSPResampler.h"

namespace MasterSDR {

Resampler::Resampler(double srcRate, double dstRate, int maxBlockSamples)
    : m_srcRate(srcRate)
    , m_dstRate(dstRate)
    , m_maxBlockSamples(maxBlockSamples)
    , m_resampler(std::make_unique<r8b::CDSPResampler24>(srcRate, dstRate, maxBlockSamples))
{
    m_inBuf.reserve(maxBlockSamples);
}

Resampler::~Resampler() = default;

QByteArray Resampler::process(const float* in, int numSamples)
{
    if (numSamples <= 0) return {};

    // r8b does not bounds-check against aMaxInLen; exceeding it silently
    // overflows internal filter buffers. Chunk so each call stays within limit.
    if (numSamples > m_maxBlockSamples) {
        QByteArray result;
        for (int offset = 0; offset < numSamples; offset += m_maxBlockSamples)
            result.append(process(in + offset, std::min(numSamples - offset, m_maxBlockSamples)));
        return result;
    }

    // Convert float32 → double
    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(in[i]);

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numSamples, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32
    QByteArray result(outLen * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(result.data());
    for (int i = 0; i < outLen; ++i)
        dst[i] = static_cast<float>(outPtr[i]);
    return result;
}

QByteArray Resampler::processStereoToMono(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    if (numStereoFrames > m_maxBlockSamples) {
        QByteArray result;
        for (int offset = 0; offset < numStereoFrames; offset += m_maxBlockSamples)
            result.append(processStereoToMono(stereoIn + offset * 2, std::min(numStereoFrames - offset, m_maxBlockSamples)));
        return result;
    }

    // Downmix stereo → mono
    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numStereoFrames, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32 mono
    QByteArray result(outLen * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(result.data());
    for (int i = 0; i < outLen; ++i)
        dst[i] = static_cast<float>(outPtr[i]);
    return result;
}

QByteArray Resampler::processMonoToStereo(const float* monoIn, int numSamples)
{
    if (numSamples <= 0) return {};

    if (numSamples > m_maxBlockSamples) {
        QByteArray result;
        for (int offset = 0; offset < numSamples; offset += m_maxBlockSamples)
            result.append(processMonoToStereo(monoIn + offset, std::min(numSamples - offset, m_maxBlockSamples)));
        return result;
    }

    // Convert float32 → double
    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(monoIn[i]);

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numSamples, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32 stereo (duplicate mono to L+R)
    QByteArray result(outLen * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(result.data());
    for (int i = 0; i < outLen; ++i) {
        float s = static_cast<float>(outPtr[i]);
        dst[2 * i]     = s;
        dst[2 * i + 1] = s;
    }
    return result;
}

QByteArray Resampler::processStereoToStereo(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    if (numStereoFrames > m_maxBlockSamples) {
        QByteArray result;
        for (int offset = 0; offset < numStereoFrames; offset += m_maxBlockSamples)
            result.append(processStereoToStereo(stereoIn + offset * 2, std::min(numStereoFrames - offset, m_maxBlockSamples)));
        return result;
    }

    // Downmix stereo → mono, resample, duplicate back to stereo
    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numStereoFrames, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    QByteArray result(outLen * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(result.data());
    for (int i = 0; i < outLen; ++i) {
        float s = static_cast<float>(outPtr[i]);
        dst[2 * i]     = s;
        dst[2 * i + 1] = s;
    }
    return result;
}

} // namespace MasterSDR
