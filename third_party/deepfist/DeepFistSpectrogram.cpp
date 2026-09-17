// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md

#include "DeepFistSpectrogram.h"

#include <cmath>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Reflect-pad index: map a padded-array index p (0-based, padded length n+2*PAD)
// back to a source index in [0, n), mirroring around the edges the way
// torch.nn.functional.pad(mode="reflect") does (edge sample not repeated).
inline int reflectIdx(int p, int n, int pad) {
    int i = p - pad;                 // index into the original signal
    if (n == 1) return 0;
    // fold repeatedly into [0, n-1] by reflection
    const int period = 2 * (n - 1);
    int m = i % period;
    if (m < 0) m += period;
    return (m < n) ? m : (period - m);
}
}  // namespace

DeepFistSpectrogram::DeepFistSpectrogram() {
    // torch.hann_window(256, periodic=True): 0.5 - 0.5*cos(2*pi*n/256)
    hann_.resize(kNFft);
    for (int i = 0; i < kNFft; ++i)
        hann_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / kNFft));

    // bit-reversal permutation for N=256 (8 bits)
    rev_.resize(kNFft);
    for (int i = 0; i < kNFft; ++i) {
        int r = 0, x = i;
        for (int b = 0; b < 8; ++b) { r = (r << 1) | (x & 1); x >>= 1; }
        rev_[i] = r;
    }

    // twiddles W_N^k = exp(-j*2*pi*k/N), k = 0..N/2-1
    tw_.resize(kNFft / 2);
    for (int k = 0; k < kNFft / 2; ++k)
        tw_[k] = std::complex<float>(
            static_cast<float>(std::cos(-2.0 * kPi * k / kNFft)),
            static_cast<float>(std::sin(-2.0 * kPi * k / kNFft)));
}

void DeepFistSpectrogram::fft256(std::complex<float>* a) const {
    // in-place iterative radix-2 DIT, N = 256
    for (int i = 0; i < kNFft; ++i) {
        int j = rev_[i];
        if (j > i) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= kNFft; len <<= 1) {
        const int half = len >> 1;
        const int step = kNFft / len;       // twiddle stride
        for (int base = 0; base < kNFft; base += len) {
            for (int k = 0; k < half; ++k) {
                std::complex<float> w = tw_[k * step];
                std::complex<float> u = a[base + k];
                std::complex<float> v = a[base + k + half] * w;
                a[base + k]        = u + v;
                a[base + k + half] = u - v;
            }
        }
    }
}

void DeepFistSpectrogram::compute(const float* audio, int n,
                                  std::vector<float>& outTile, int& T) {
    T = 0;
    outTile.clear();
    if (!audio || n <= 0) return;

    constexpr int pad = kNFft / 2;          // 128, center=True padding
    T = 1 + n / kHop;                       // torch center framing

    outTile.assign(static_cast<size_t>(kFreqBins) * T, 0.0f);

    std::vector<std::complex<float>> frame(kNFft);

    double mean = 0.0, m2 = 0.0;            // Welford running mean / M2
    long   count = 0;

    for (int t = 0; t < T; ++t) {
        const int start = t * kHop;         // start index in the padded array
        for (int k = 0; k < kNFft; ++k) {
            const int si = reflectIdx(start + k, n, pad);
            frame[k] = std::complex<float>(audio[si] * hann_[k], 0.0f);
        }
        fft256(frame.data());
        for (int f = 0; f < kFreqBins; ++f) {
            const std::complex<float>& c = frame[kBinLo + f];
            const float mag  = std::sqrt(c.real() * c.real() + c.imag() * c.imag());
            const float val  = std::log1p(mag);
            outTile[static_cast<size_t>(f) * T + t] = val;
            // running stats for the global standardize
            ++count;
            const double d = val - mean;
            mean += d / count;
            m2   += d * (val - mean);
        }
    }

    // global standardize: (x - mean) / (std + 1e-6), unbiased std (torch default)
    const double var = (count > 1) ? m2 / (count - 1) : 0.0;
    const double inv = 1.0 / (std::sqrt(var) + 1e-6);
    for (float& v : outTile)
        v = static_cast<float>((v - mean) * inv);
}

}  // namespace lyra::dsp
