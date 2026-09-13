#include "ClientEqFftAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace AetherSDR {

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

float ClientEqFftAnalyzer::coherentGainCorrectionDb() const noexcept
{
    // Mean of the window = its coherent gain. Summed in double because 2048
    // float adds of ~0.5 each would lose the last digits of a figure whose
    // whole job is to be exact.
    double sum = 0.0;
    for (const float w : m_window)
        sum += static_cast<double>(w);
    const double mean = sum / static_cast<double>(kFftSize);
    if (!(mean > 1e-12))
        return 0.0f;   // a degenerate window has no meaningful correction
    return static_cast<float>(-20.0 * std::log10(mean));
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

    // Magnitude → dB. The 2/N here is the single-sided normalisation for an
    // UNWINDOWED transform and nothing more: it does NOT absorb the Hann
    // window's 0.5 coherent gain, whatever the comment that used to sit here
    // claimed. The scale this produces is therefore 6.02 dB below true dBFS —
    // a full-scale sine peaks at -6.02, not at 0.
    //
    // Left that way on purpose. The EQ editor has drawn this scale since it
    // shipped and only ever reads it as a shape, so changing `norm` would move
    // a display for every existing user to fix a number none of them reads. A
    // caller that states an ABSOLUTE level adds coherentGainCorrectionDb()
    // instead; `bandscope_analyzer_test` pins both scales so neither can drift.
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

} // namespace AetherSDR
