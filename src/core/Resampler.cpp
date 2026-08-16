#include "Resampler.h"

#include "CDSPResampler.h"

#include <algorithm>

namespace AetherSDR {

Resampler::Resampler(double srcRate, double dstRate, int maxBlockSamples, double reqTransBand)
    : m_srcRate(srcRate)
    , m_dstRate(dstRate)
    , m_maxBlockSamples(maxBlockSamples)
    , m_resampler(std::make_unique<r8b::CDSPResampler24>(srcRate, dstRate, maxBlockSamples, reqTransBand))
    , m_groupDelayInputFrames(m_resampler->getInLenBeforeOutPos(0))
{
    m_inBuf.reserve(maxBlockSamples);
    prewarm();
}

Resampler::~Resampler() = default;

QByteArray Resampler::process(const float* in, int numSamples)
{
    QByteArray result;
    process(in, numSamples, result);
    return result;
}

int Resampler::process(const float* in, int numSamples, QByteArray& output)
{
    output.resize(0);
    if (!in || numSamples <= 0) {
        return 0;
    }

    // r8b does not bounds-check against aMaxInLen; exceeding it silently
    // overflows internal filter buffers. Feed bounded chunks while appending
    // directly into the caller's reusable output storage.
    int totalOutputSamples = 0;
    for (int offset = 0; offset < numSamples; offset += m_maxBlockSamples) {
        const int chunkSamples = std::min(
            numSamples - offset, m_maxBlockSamples);
        m_inBuf.resize(chunkSamples);
        for (int index = 0; index < chunkSamples; ++index) {
            m_inBuf[static_cast<size_t>(index)] =
                static_cast<double>(in[offset + index]);
        }

        double* outputPointer = nullptr;
        const int outputSamples = m_resampler->process(
            m_inBuf.data(), chunkSamples, outputPointer);
        if (outputSamples <= 0 || !outputPointer) {
            continue;
        }

        const int oldBytes = output.size();
        output.resize(oldBytes + outputSamples * static_cast<int>(sizeof(float)));
        auto* destination = reinterpret_cast<float*>(output.data() + oldBytes);
        for (int index = 0; index < outputSamples; ++index) {
            destination[index] = static_cast<float>(outputPointer[index]);
        }
        totalOutputSamples += outputSamples;
    }
    return totalOutputSamples;
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

void Resampler::reset()
{
    m_resampler->clear();
    prewarm();
}

void Resampler::prewarm()
{
    if (std::abs(m_srcRate - m_dstRate) < 0.001) return;

    // Called from the constructor for every Resampler instance (RX, TX, BNR, RADE, etc.).
    // r8brain with DoConsumeLatency=true silently discards the first Latency input samples
    // before emitting any output. Feeding zeros here consumes that startup latency so the
    // first real audio sample produces output immediately, removing the transient that would
    // otherwise appear at the start of every audio session.
    double* outPtr = nullptr;
    int lenRequired = m_groupDelayInputFrames;
    while (lenRequired > 0) {
        const int len = std::min(lenRequired, m_maxBlockSamples);
        m_inBuf.resize(static_cast<size_t>(len));
        std::fill(m_inBuf.begin(), m_inBuf.end(), 0.0);
        m_resampler->process(m_inBuf.data(), len, outPtr);
        lenRequired -= len;
    }
}

} // namespace AetherSDR
