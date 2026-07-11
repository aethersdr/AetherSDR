#pragma once

#include <QByteArray>

namespace AetherSDR {

// Re-applies a mono DSP path's gain envelope to the delayed dry stereo signal.
// This keeps a single mono noise analysis while preserving RX left/right balance.
class MonoDspStereoAdapter {
public:
    explicit MonoDspStereoAdapter(int processingLatencyFrames = 0);

    void reset();
    void setProcessingLatencyFrames(int frames);

    void pushDryStereo(const QByteArray& stereoPcm);
    QByteArray takeProcessedMono(const float* processedMono, int frames);

    int bufferedFrames() const;

private:
    int readableDryStereoBytes() const;
    void compactDryStereoFifoIfNeeded();
    void resetEnvelopeState();

    QByteArray m_dryStereoFifo;
    int m_dryStereoReadOffset{0};
    int m_processingLatencyFrames{0};
    int m_latencyFramesRemaining{0};
    float m_dryMonoPower{0.0f};
    float m_dryStereoPower{0.0f};
    float m_sidePower{0.0f};
    float m_processedPower{0.0f};
    float m_gain{1.0f};
    float m_processedMix{1.0f};
};

} // namespace AetherSDR
