// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md
//
// Lyra — DeepFist neural CW decoder: audio -> log-magnitude spectrogram.
//
// Faithful port of DeepFist's MIT front-end (deepfist/features/spectrogram.py):
//   STFT n_fft=256, hop=48, Hann window, center=True (reflect pad n_fft/2),
//   |mag|, keep the 65 bins spanning 400-1200 Hz at 3200 Hz, log1p, then a
//   PER-WINDOW global standardize:  (x - mean) / (std + 1e-6).
// There are no fixed normalisation constants — mean/std are computed over the
// whole 65 x T tile each call, exactly as the Python does.
//
// Output layout matches the ONNX input tensor [batch, 1, freq=65, time]:
// row-major freq-major, element (f, t) at outTile[f * T + t].
//
// The FFT is a from-scratch radix-2 implementation; no third-party (and no
// AGPL) code is used.
#pragma once

#include <complex>
#include <vector>

namespace lyra::dsp {

class DeepFistSpectrogram {
public:
    static constexpr int kNFft     = 256;
    static constexpr int kHop      = 48;
    static constexpr int kFreqBins = 65;    // bins 32..96 (400-1200 Hz @3200)
    static constexpr int kBinLo    = 32;    // ceil(400 / 12.5)

    DeepFistSpectrogram();

    // audio: mono @ 3200 Hz, `n` samples.  Fills `outTile` (resized to 65*T)
    // and sets `T` = number of STFT frames = 1 + n/hop.
    void compute(const float* audio, int n, std::vector<float>& outTile, int& T);

private:
    void fft256(std::complex<float>* a) const;   // in-place radix-2, N=256

    std::vector<float>          hann_;    // 256 Hann-window coefficients
    std::vector<int>            rev_;      // bit-reversal permutation (256)
    std::vector<std::complex<float>> tw_;  // twiddle factors
};

}  // namespace lyra::dsp
