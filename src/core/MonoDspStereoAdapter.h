#pragma once

#include <QByteArray>

namespace AetherSDR {

// Applies a delayed dry stereo balance to a mono DSP waveform. This keeps a
// single mono noise analysis while preserving RX left/right levels without
// feeding dry program material back into the processed output. Independent
// stereo content cannot survive a mono DSP path; only its level balance does.
class MonoDspStereoAdapter {
public:
    explicit MonoDspStereoAdapter(int processingLatencyFrames = 0, int sampleRate = 24000);

    bool isValid() const { return m_sampleRate == 24000 || m_sampleRate == 48000; }
    int sampleRate() const { return m_sampleRate; }

    void reset();
    void setProcessingLatencyFrames(int frames);

    void pushDryStereo(const QByteArray& stereoPcm);
    QByteArray takeProcessedMono(const float* processedMono, int frames);

    int bufferedFrames() const;

private:
    int readableDryStereoBytes() const;
    void compactDryStereoFifoIfNeeded();
    void resetEnvelopeState();

    int m_sampleRate{24000};
    float m_balanceEnvelopeCoeff{4.0e-5f};
    float m_monoObservabilityEnvelopeCoeff{0.006f};
    float m_balancePowerFloor{0.0f};
    QByteArray m_dryStereoFifo;
    int m_dryStereoReadOffset{0};
    int m_processingLatencyFrames{0};
    int m_latencyFramesRemaining{0};
    float m_dryMonoPower{0.0f};
    float m_leftPower{0.0f};
    float m_rightPower{0.0f};
    float m_dryStereoPower{0.0f};
    float m_leftBalance{1.0f};
    float m_rightBalance{1.0f};
};

} // namespace AetherSDR
