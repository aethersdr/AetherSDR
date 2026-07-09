#pragma once

#include <QByteArray>

namespace AetherSDR {

// Re-applies a mono DSP path's gain envelope to the delayed dry stereo signal.
// This keeps a single mono noise analysis while preserving RX left/right balance.
class MonoDspStereoAdapter {
public:
    void reset();

    void pushDryStereo(const QByteArray& stereoPcm);
    QByteArray takeProcessedMono(const float* processedMono, int frames);

    int bufferedFrames() const;

private:
    QByteArray m_dryStereoFifo;
    float m_dryMonoPower{0.0f};
    float m_dryStereoPower{0.0f};
    float m_processedPower{0.0f};
    float m_gain{1.0f};
};

} // namespace AetherSDR
