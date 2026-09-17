#pragma once

#include <array>
#include <vector>

namespace AetherSDR {

// Small, self-contained FFT analyzer used by the Client EQ editor to
// render a live spectrum behind the response curve. Fixed 2048-point
// radix-2 Cooley-Tukey — bin resolution fs/N = 11.7 Hz at 24 kHz, so
// the first (non-DC) bin lands below the 20 Hz display floor and the
// analyzer does not produce a visible "cutoff" artifact at the
// leftmost visible frequency. Runs in ~200 µs on the UI thread, cheap
// enough for a 25 Hz timer.
//
// Usage:
//   ClientEqFftAnalyzer fft;
//   fft.update(samples, ClientEqFftAnalyzer::kFftSize);  // from audio tap
//   for (auto db : fft.magnitudesDb()) ...
//
// Magnitude bins are exponentially smoothed per-bin with asymmetric
// attack (fast) and decay (slow) — the classic "analyzer follow" feel.
class ClientEqFftAnalyzer {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kBinCount = kFftSize / 2 + 1;  // 0 Hz .. Nyquist

    ClientEqFftAnalyzer();

    // Feed the most-recent kFftSize samples. The window (Hann) and the
    // FFT run inline; smoothed magnitudes are updated afterwards.
    void update(const float* samples, int count) noexcept;

    // Reset smoothing state — e.g. when the editor hides so the next
    // opening doesn't show frozen bars from the last session.
    void reset() noexcept;

    // Magnitudes in dB, length kBinCount. Floor is kFloorDb to keep the
    // log curve from collapsing to -infinity at silent bins.
    const std::vector<float>& magnitudesDb() const { return m_smoothedDb; }

    // Frequency of bin index i for a given sample rate.
    static float binFreq(int bin, double sampleRate) {
        return static_cast<float>(bin * sampleRate / kFftSize);
    }

    // The dB to ADD to magnitudesDb() before reading a bin as an absolute
    // level. +6.02 dB for the Hann window this class builds.
    //
    // WHY A CALLER HAS TO DO THIS. update() normalises by 2/N, which is the
    // single-sided normalisation for an UNWINDOWED transform. It does not
    // remove the analysis window's coherent gain — the window's mean, 1/2 for
    // Hann — so a full-scale sine peaks at -6.02 dBFS and not at 0. That is
    // invisible to the EQ editor, which draws a spectrum's shape against a
    // response curve and never names an absolute number, and it is why the
    // error survived here; it is NOT invisible to a caller that puts a dBFS
    // figure on screen beside a converter's clip threshold.
    //
    // Offered as a correction for the caller to apply rather than folded into
    // `norm`, because folding it in would move the EQ editor's displayed
    // spectrum 6 dB for every existing user of that window, which is a
    // separate decision from getting one new readout right.
    //
    // Computed from the window this instance actually built, so a change to
    // buildWindow() carries the constant with it instead of stranding a 6.02
    // somewhere else in the tree.
    float coherentGainCorrectionDb() const noexcept;

    static constexpr float kFloorDb = -100.0f;

private:
    void buildWindow();

    std::array<float, kFftSize>   m_window;    // precomputed Hann
    std::vector<float>            m_smoothedDb; // size kBinCount
    bool                          m_primed{false};
};

} // namespace AetherSDR
