#include "MonoDspStereoAdapter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {

constexpr int kChannels = 2;
constexpr int kSampleRate = 24000;
constexpr int kMaxBufferedFrames = kSampleRate * 5;
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
    m_dryMonoPower = 0.0f;
    m_dryStereoPower = 0.0f;
    m_processedPower = 0.0f;
    m_gain = 1.0f;
}

void MonoDspStereoAdapter::pushDryStereo(const QByteArray& stereoPcm)
{
    if (stereoPcm.isEmpty()) {
        return;
    }

    m_dryStereoFifo.append(stereoPcm);

    const int maxBytes = kMaxBufferedFrames * kChannels * static_cast<int>(sizeof(float));
    if (m_dryStereoFifo.size() > maxBytes) {
        const int extraBytes = m_dryStereoFifo.size() - maxBytes;
        const int alignedExtraBytes =
            extraBytes - (extraBytes % (kChannels * static_cast<int>(sizeof(float))));
        if (alignedExtraBytes > 0) {
            m_dryStereoFifo.remove(0, alignedExtraBytes);
        }
    }
}

QByteArray MonoDspStereoAdapter::takeProcessedMono(const float* processedMono, int frames)
{
    if (!processedMono || frames <= 0) {
        return {};
    }

    const int outputBytes = frames * kChannels * static_cast<int>(sizeof(float));
    QByteArray output(outputBytes, Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(output.data());

    const int availableFrames =
        m_dryStereoFifo.size() / (kChannels * static_cast<int>(sizeof(float)));
    const int dryFrames = std::min(frames, availableFrames);
    const auto* dry = reinterpret_cast<const float*>(m_dryStereoFifo.constData());

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
        m_dryStereoFifo.remove(
            0, dryFrames * kChannels * static_cast<int>(sizeof(float)));
    }

    return output;
}

int MonoDspStereoAdapter::bufferedFrames() const
{
    return m_dryStereoFifo.size() / (kChannels * static_cast<int>(sizeof(float)));
}

} // namespace AetherSDR
