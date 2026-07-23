#pragma once

#include <vector>

namespace AetherSDR {

// Kaldi-compatible 80-dim log-Mel filterbank (Fbank) front-end for the speaker
// embedder (RFC #4333 follow-up). WeSpeaker / 3D-Speaker ONNX models consume
// these features (input tensor "feats"), not raw audio.
//
// Matches torchaudio.compliance.kaldi.fbank defaults used by WeSpeaker:
// 16 kHz, 80 mel bins, 25 ms window / 10 ms shift, Povey window, pre-emphasis
// 0.97, DC removal, power spectrum, log, 20 Hz..Nyquist — followed by per-
// utterance cepstral mean normalization (subtract each bin's mean over time),
// which WeSpeaker applies before the model. Self-contained (own radix-2 FFT),
// so it adds no dependency to aetherasr.
class Fbank {
public:
    Fbank();

    static constexpr int kNumBins = 80;

    // Compute features for one utterance of 16 kHz mono float samples in [-1, 1].
    // Returns a row-major [numFrames][80] buffer (size = numFrames*80) with CMN
    // applied; numFrames is written to *frames. Empty if the input is too short.
    std::vector<float> compute(const float* samples, int count, int* frames) const;

private:
    void fft512(float* re, float* im) const;

    std::vector<float> m_window;                 // Povey window, kFrameLen
    std::vector<std::vector<float>> m_melWeights; // [80][kFftBins] triangular filters
};

} // namespace AetherSDR
