#pragma once

#include <vector>

namespace AetherSDR {

// Kaldi-compatible 80-dim log-Mel filterbank for the speaker embedder (ONNX input
// "feats"). Matches torchaudio.compliance.kaldi.fbank as WeSpeaker uses it: 16 kHz,
// 25 ms / 10 ms, Povey window, pre-emphasis 0.97, DC removal, power spectrum, log,
// 20 Hz..Nyquist, then per-utterance cepstral mean normalization. Own radix-2 FFT.
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
