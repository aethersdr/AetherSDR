#include "MonoDspStereoAdapter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {

constexpr int kChannels = 2;
constexpr int kFrameBytes = kChannels * static_cast<int>(sizeof(float));
// Keep balance changes well below the audio envelope rate. The mono DSP owns
// the program waveform; this estimate only distributes it between channels.
constexpr float kBalanceEnvelopeCoeff = 4.0e-5f;
constexpr float kMonoObservabilityEnvelopeCoeff = 0.006f;
constexpr float kPowerFloor = 1.0e-10f;
// Keep the same effective input threshold when the envelope coefficient changes.
constexpr float kBalancePowerFloor =
    kPowerFloor * (kBalanceEnvelopeCoeff / kMonoObservabilityEnvelopeCoeff);
constexpr float kMonoObservabilityRatio = 0.01f;

float clampSample(float sample)
{
    return std::clamp(sample, -1.0f, 1.0f);
}

float updatePowerEnvelope(
    float current, float samplePower, float coefficient, float floor)
{
    current += coefficient * (samplePower - current);
    return current < floor ? 0.0f : current;
}

} // namespace

MonoDspStereoAdapter::MonoDspStereoAdapter(int processingLatencyFrames, int sampleRate)
    : m_sampleRate(sampleRate)
    , m_processingLatencyFrames(std::max(0, processingLatencyFrames))
    , m_latencyFramesRemaining(m_processingLatencyFrames)
{
    // Preserve the legacy coefficients bit-for-bit. At 48 kHz two updates
    // cover the same elapsed time as one legacy update.
    if (sampleRate == 48000) {
        m_balanceEnvelopeCoeff = 1.0f - std::sqrt(1.0f - kBalanceEnvelopeCoeff);
        m_monoObservabilityEnvelopeCoeff =
            1.0f - std::sqrt(1.0f - kMonoObservabilityEnvelopeCoeff);
    }
    m_balancePowerFloor = sampleRate == 24000 ? kBalancePowerFloor
        : kPowerFloor * (m_balanceEnvelopeCoeff / m_monoObservabilityEnvelopeCoeff);
}

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
    m_leftPower = 0.0f;
    m_rightPower = 0.0f;
    m_dryStereoPower = 0.0f;
    m_leftBalance = 1.0f;
    m_rightBalance = 1.0f;
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

    if (m_dryStereoReadOffset >= m_sampleRate * kFrameBytes) {
        m_dryStereoFifo.remove(0, m_dryStereoReadOffset);
        m_dryStereoReadOffset = 0;
    }
}

void MonoDspStereoAdapter::pushDryStereo(const QByteArray& stereoPcm)
{
    if (!isValid() || stereoPcm.isEmpty()) {
        return;
    }

    m_dryStereoFifo.append(stereoPcm);

    const int readableBytes = readableDryStereoBytes();
    if (readableBytes > m_sampleRate * 5 * kFrameBytes) {
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
    if (!isValid() || !processedMono || frames <= 0) {
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
            dst[i * kChannels] = clampSample(processed * m_leftBalance);
            dst[i * kChannels + 1] = clampSample(processed * m_rightBalance);
            --m_latencyFramesRemaining;
            continue;
        }

        if (dryFrames >= availableFrames) {
            dst[i * kChannels] = clampSample(processed * m_leftBalance);
            dst[i * kChannels + 1] = clampSample(processed * m_rightBalance);
            continue;
        }

        const float left = dry[dryFrames * kChannels];
        const float right = dry[dryFrames * kChannels + 1];
        const float dryMono = 0.5f * (left + right);
        const float dryStereoPower = 0.5f * (left * left + right * right);

        m_dryMonoPower = updatePowerEnvelope(
            m_dryMonoPower,
            dryMono * dryMono,
            m_monoObservabilityEnvelopeCoeff,
            kPowerFloor);
        m_dryStereoPower = updatePowerEnvelope(
            m_dryStereoPower,
            dryStereoPower,
            m_monoObservabilityEnvelopeCoeff,
            kPowerFloor);

        const float observableMonoFloor =
            std::max(kPowerFloor, m_dryStereoPower * kMonoObservabilityRatio);
        const float instantObservableMonoFloor =
            std::max(kPowerFloor, dryStereoPower * kMonoObservabilityRatio);
        // If stereo energy is present but the mono sum cancels, there is no
        // trustworthy denominator for a balance estimate. Keep the last valid
        // balance instead of reintroducing dry audio or amplifying noise
        // through an unstable ratio.
        if (dryMono * dryMono >= instantObservableMonoFloor
            && m_dryMonoPower >= observableMonoFloor) {
            m_leftPower = updatePowerEnvelope(
                m_leftPower,
                left * left,
                m_balanceEnvelopeCoeff,
                m_balancePowerFloor);
            m_rightPower = updatePowerEnvelope(
                m_rightPower,
                right * right,
                m_balanceEnvelopeCoeff,
                m_balancePowerFloor);
            const float leftLevel = std::sqrt(std::max(m_leftPower, 0.0f));
            const float rightLevel = std::sqrt(std::max(m_rightPower, 0.0f));
            const float totalLevel = leftLevel + rightLevel;
            if (totalLevel > 0.0f) {
                m_leftBalance = 2.0f * leftLevel / totalLevel;
                m_rightBalance = 2.0f * rightLevel / totalLevel;
            }
        }
        dst[i * kChannels] = clampSample(processed * m_leftBalance);
        dst[i * kChannels + 1] = clampSample(processed * m_rightBalance);
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
