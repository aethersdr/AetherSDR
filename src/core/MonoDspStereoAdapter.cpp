#include "MonoDspStereoAdapter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {

constexpr int kChannels = 2;
constexpr int kSampleRate = 24000;
constexpr int kMaxBufferedFrames = kSampleRate * 5;
constexpr int kFrameBytes = kChannels * static_cast<int>(sizeof(float));
constexpr int kMaxBufferedBytes = kMaxBufferedFrames * kFrameBytes;
constexpr int kCompactThresholdBytes = kSampleRate * kFrameBytes;
constexpr float kEnvelopeCoeff = 0.006f;
constexpr float kAttackCoeff = 0.22f;
constexpr float kReleaseCoeff = 0.035f;
constexpr float kPowerFloor = 1.0e-10f;
constexpr float kMonoObservabilityRatio = 0.01f;
constexpr float kMinObservableGain = 0.035f;
constexpr float kMaxGain = 1.0f;

float clampSample(float sample)
{
    return std::clamp(sample, -1.0f, 1.0f);
}

} // namespace

void MonoDspStereoAdapter::reset()
{
    m_dryStereoFifo.clear();
    m_dryStereoReadOffset = 0;
    m_dryMonoPower = 0.0f;
    m_dryStereoPower = 0.0f;
    m_processedPower = 0.0f;
    m_gain = 1.0f;
}

int MonoDspStereoAdapter::readableDryStereoBytes() const
{
    const qsizetype readableBytes =
        m_dryStereoFifo.size() - static_cast<qsizetype>(m_dryStereoReadOffset);
    return readableBytes > 0 ? static_cast<int>(readableBytes) : 0;
}

void MonoDspStereoAdapter::compactDryStereoFifoIfNeeded()
{
    if (m_dryStereoReadOffset <= 0) {
        return;
    }

    if (m_dryStereoReadOffset >= m_dryStereoFifo.size()) {
        m_dryStereoFifo.clear();
        m_dryStereoReadOffset = 0;
        return;
    }

    if (m_dryStereoReadOffset >= kCompactThresholdBytes) {
        m_dryStereoFifo.remove(0, m_dryStereoReadOffset);
        m_dryStereoReadOffset = 0;
    }
}

void MonoDspStereoAdapter::pushDryStereo(const QByteArray& stereoPcm)
{
    if (stereoPcm.isEmpty()) {
        return;
    }

    m_dryStereoFifo.append(stereoPcm);

    const int readableBytes = readableDryStereoBytes();
    if (readableBytes > kMaxBufferedBytes) {
        const int extraBytes = readableBytes - kMaxBufferedBytes;
        const int alignedExtraBytes = extraBytes - (extraBytes % kFrameBytes);
        if (alignedExtraBytes > 0) {
            m_dryStereoReadOffset += alignedExtraBytes;
        }
    }

    compactDryStereoFifoIfNeeded();
}

QByteArray MonoDspStereoAdapter::takeProcessedMono(const float* processedMono, int frames)
{
    if (!processedMono || frames <= 0) {
        return {};
    }

    const int outputBytes = frames * kFrameBytes;
    QByteArray output(outputBytes, Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(output.data());

    const int availableFrames = readableDryStereoBytes() / kFrameBytes;
    const int dryFrames = std::min(frames, availableFrames);
    const auto* dry = reinterpret_cast<const float*>(
        m_dryStereoFifo.constData() + m_dryStereoReadOffset);

    for (int i = 0; i < dryFrames; ++i) {
        const float left = dry[i * kChannels];
        const float right = dry[i * kChannels + 1];
        const float dryMono = 0.5f * (left + right);
        const float dryStereoPower = 0.5f * (left * left + right * right);
        const float processed = processedMono[i];

        m_dryMonoPower += kEnvelopeCoeff * (dryMono * dryMono - m_dryMonoPower);
        m_dryStereoPower += kEnvelopeCoeff * (dryStereoPower - m_dryStereoPower);
        m_processedPower += kEnvelopeCoeff * (processed * processed - m_processedPower);

        float targetGain = kMaxGain;
        const float observableMonoFloor =
            std::max(kPowerFloor, m_dryStereoPower * kMonoObservabilityRatio);
        // If stereo energy is present but the mono sum cancels, the mono DSP
        // path has no trustworthy gain estimate. Preserve the dry stereo level.
        if (m_dryMonoPower >= observableMonoFloor) {
            targetGain = std::clamp(
                std::sqrt(std::max(m_processedPower, 0.0f) / m_dryMonoPower),
                kMinObservableGain,
                kMaxGain);
        }
        const float smoothCoeff = targetGain < m_gain ? kAttackCoeff : kReleaseCoeff;
        m_gain += smoothCoeff * (targetGain - m_gain);

        dst[i * kChannels] = clampSample(left * m_gain);
        dst[i * kChannels + 1] = clampSample(right * m_gain);
    }

    for (int i = dryFrames; i < frames; ++i) {
        dst[i * kChannels] = 0.0f;
        dst[i * kChannels + 1] = 0.0f;
    }

    if (dryFrames > 0) {
        m_dryStereoReadOffset += dryFrames * kFrameBytes;
        compactDryStereoFifoIfNeeded();
    }

    return output;
}

int MonoDspStereoAdapter::bufferedFrames() const
{
    return readableDryStereoBytes() / kFrameBytes;
}

} // namespace AetherSDR
