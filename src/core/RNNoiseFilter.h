#pragma once

#include "MonoDspStereoAdapter.h"

#include <QByteArray>
#include <memory>
#include <vector>

struct DenoiseState;

namespace AetherSDR {

class Resampler;

// Client-side RNN noise suppression using Mozilla/Xiph RNNoise.
// Processes 24kHz duplicated-stereo FLOAT32 audio by upsampling to
// 48kHz mono, running RNNoise, and downsampling back to 24kHz stereo.
//
// RNNoise requires 48kHz mono float input in 480-sample (10ms) frames.

class RNNoiseFilter {
public:
    enum class OutputMode {
        PreserveRxStereo,
        ProcessedMono,
    };

    explicit RNNoiseFilter(OutputMode outputMode = OutputMode::PreserveRxStereo);
    ~RNNoiseFilter();

    // Process a block of 24kHz duplicated-stereo FLOAT32 PCM (NOT int16
    // — the original comment lied; callers must pass float32 sample
    // pairs interleaved as L,R,L,R,... with each sample in [-1.0, 1.0]).
    // Returns the processed block in the same format and frame count.
    QByteArray process(const QByteArray& pcm24kStereo);

    // Returns true if rnnoise_create() succeeded.
    bool isValid() const { return m_state != nullptr; }

    // Reset internal state (e.g., on band change).
    void reset();

private:
    DenoiseState* m_state{nullptr};
    std::unique_ptr<Resampler> m_up;    // 24kHz mono → 48kHz mono
    std::unique_ptr<Resampler> m_down;  // 48kHz mono → 24kHz mono
    QByteArray    m_inAccum;            // accumulate 48kHz mono float input
    QByteArray    m_outAccum;           // accumulate 24kHz stereo float output
    std::vector<float> m_mono24k;
    std::vector<float> m_processed48k;
    std::vector<float> m_processed48kFloat;
    MonoDspStereoAdapter m_stereoAdapter;
    OutputMode m_outputMode{OutputMode::PreserveRxStereo};
};

} // namespace AetherSDR
