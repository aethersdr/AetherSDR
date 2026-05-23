#include "ClientEqFftAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace MasterSDR {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Attack and release alphas for the one-pole per-bin smoother. Fast
// attack captures transients; slow release gives the display a natural
// decay that matches what the ear hears.
constexpr float kAttackAlpha  = 0.45f;
constexpr float kReleaseAlpha = 0.10f;

// In-place radix-2 Cooley-Tukey FFT on std::complex<float>.  N must be
// a power of two; at N=256 this is ~20 µs on modern x86 — plenty cheap
// to run on a 25 Hz UI timer.
void fftInPlace(std::complex<float>* data, int n)
{
    // Bit-reversal permutation
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    // Butterflies
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -kTwoPi / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k) {
                const auto u = data[i + k];
                const auto v = data[i + k + len / 2] * w;
                data[i + k]             = u + v;
                data[i + k + len / 2]   = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace

ClientEqFftAnalyzer::ClientEqFftAnalyzer()
    : m_smoothedDb(kBinCount, kFloorDb)
{
    buildWindow();
}

void ClientEqFftAnalyzer::buildWindow()
{
    for (int i = 0; i < kFftSize; ++i) {
        m_window[i] = 0.5f * (1.0f - std::cos(kTwoPi * static_cast<float>(i)
                                              / static_cast<float>(kFftSize - 1)));
    }
}

void ClientEqFftAnalyzer::reset() noexcept
{
    std::fill(m_smoothedDb.begin(), m_smoothedDb.end(), kFloorDb);
    m_primed = false;
}

void ClientEqFftAnalyzer::update(const float* samples, int count) noexcept
{
    if (count < kFftSize || !samples) return;

    // Take the most-recent kFftSize samples. The audio tap fills
    // newest-last, so we slice from the tail.
    const float* window = samples + (count - kFftSize);

    // Window and promote to complex.
    std::complex<float> buf[kFftSize];
    for (int i = 0; i < kFftSize; ++i) {
        buf[i] = std::complex<float>(window[i] * m_window[i], 0.0f);
    }
    fftInPlace(buf, kFftSize);

    // Magnitude → dBFS (0 dB = full-scale sine). Normalise by N/2 to
    // land full-scale inputs at ~0 dB; Hann window reduces coherent
    // gain by 6 dB which we absorb into the normalisation.
    const float norm = 2.0f / static_cast<float>(kFftSize);
    for (int i = 0; i < kBinCount; ++i) {
        const float mag = std::abs(buf[i]) * norm;
        const float db  = (mag > 1e-12f) ? 20.0f * std::log10(mag) : kFloorDb;

        float prev = m_smoothedDb[i];
        if (!m_primed) prev = db;
        const float alpha = (db > prev) ? kAttackAlpha : kReleaseAlpha;
        m_smoothedDb[i] = prev + alpha * (db - prev);
    }
    m_primed = true;
}

} // namespace MasterSDR
