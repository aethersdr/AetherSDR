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
constexpr float kStereoDetectionRatio = 1.0e-4f;
constexpr float kProcessedMixAttackCoeff = 0.05f;
constexpr float kProcessedMixReleaseCoeff = 0.01f;

float clampSample(float sample)
{
    return std::clamp(sample, -1.0f, 1.0f);
}

float updatePowerEnvelope(float current, float samplePower)
{
    current += kEnvelopeCoeff * (samplePower - current);
    return current < kPowerFloor ? 0.0f : current;
}

} // namespace

MonoDspStereoAdapter::MonoDspStereoAdapter(int processingLatencyFrames)
    : m_processingLatencyFrames(std::max(0, processingLatencyFrames))
    , m_latencyFramesRemaining(m_processingLatencyFrames)
{}

void MonoDspStereoAdapter::reset()
{
    m_dryStereoFifo.clear();
    m_dryStereoReadOffset = 0;
    m_latencyFramesRemaining = m_processingLatencyFrames;
    resetEnvelopeState();
}

void MonoDspStereoAdapter::setProcessingLatencyFrames(int frames)
{
    m_processingLatencyFrames = std::max(0, frames);
    reset();
}

void MonoDspStereoAdapter::resetEnvelopeState()
{
    m_dryMonoPower = 0.0f;
    m_dryStereoPower = 0.0f;
    m_sidePower = 0.0f;
    m_processedPower = 0.0f;
    m_gain = 1.0f;
    m_processedMix = 1.0f;
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
        // A rate mismatch this large means dry and processed timelines can no
        // longer be paired reliably. Drop the pending dry side and re-prime on
        // the next block instead of preserving a permanent offset.
        m_dryStereoFifo.clear();
        m_dryStereoReadOffset = 0;
        m_latencyFramesRemaining = m_processingLatencyFrames;
        resetEnvelopeState();
        return;
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
    const auto* dry = reinterpret_cast<const float*>(
        m_dryStereoFifo.constData() + m_dryStereoReadOffset);
    int dryFrames = 0;

    for (int i = 0; i < frames; ++i) {
        const float processed = processedMono[i];
        if (m_latencyFramesRemaining > 0) {
            // Preserve the engine's own startup output while retaining dry
            // samples until the processed stream reaches the same timeline.
            dst[i * kChannels] = clampSample(processed);
            dst[i * kChannels + 1] = clampSample(processed);
            --m_latencyFramesRemaining;
            continue;
        }

        if (dryFrames >= availableFrames) {
            dst[i * kChannels] = clampSample(processed);
            dst[i * kChannels + 1] = clampSample(processed);
            continue;
        }

        const float left = dry[dryFrames * kChannels];
        const float right = dry[dryFrames * kChannels + 1];
        const float dryMono = 0.5f * (left + right);
        const float side = 0.5f * (left - right);
        const float dryStereoPower = 0.5f * (left * left + right * right);

        m_dryMonoPower = updatePowerEnvelope(m_dryMonoPower, dryMono * dryMono);
        m_dryStereoPower = updatePowerEnvelope(m_dryStereoPower, dryStereoPower);
        m_sidePower = updatePowerEnvelope(m_sidePower, side * side);
        m_processedPower = updatePowerEnvelope(m_processedPower, processed * processed);

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

        const float stereoRatio = m_sidePower
            / std::max(m_dryStereoPower, kPowerFloor);
        const float targetProcessedMix =
            stereoRatio <= kStereoDetectionRatio ? 1.0f : 0.0f;
        const float mixCoeff = targetProcessedMix < m_processedMix
            ? kProcessedMixAttackCoeff
            : kProcessedMixReleaseCoeff;
        m_processedMix += mixCoeff * (targetProcessedMix - m_processedMix);
        if (std::fabs(m_processedMix - targetProcessedMix) < 1.0e-6f) {
            m_processedMix = targetProcessedMix;
        }

        const float envelopeMix = 1.0f - m_processedMix;
        dst[i * kChannels] = clampSample(
            m_processedMix * processed + envelopeMix * left * m_gain);
        dst[i * kChannels + 1] = clampSample(
            m_processedMix * processed + envelopeMix * right * m_gain);
        ++dryFrames;
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
