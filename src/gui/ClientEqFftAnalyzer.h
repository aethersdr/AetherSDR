#pragma once

#include <array>
#include <vector>

namespace AetherSDR {

// FFT analyzer for the Client EQ editor's live spectrum. Fixed 2048-point
// radix-2: bin width fs/N = 11.7 Hz at 24 kHz, so the first non-DC bin sits
// below the 20 Hz display floor (no visible cutoff at the left edge). ~200 µs
// on the UI thread, fine for a 25 Hz timer. Bins are smoothed per bin with
// fast attack and slow decay.
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

    // dB to ADD to magnitudesDb() to read a bin as an absolute level (+6.02 dB
    // for Hann). update() normalises by 2/N, which leaves the window's coherent
    // gain (its mean, 1/2 for Hann) in place; it is not folded into `norm` so the
    // EQ editor's display does not shift. Computed from the window actually
    // built, so a buildWindow() change carries the constant with it.
    float coherentGainCorrectionDb() const noexcept;

    static constexpr float kFloorDb = -100.0f;

private:
    void buildWindow();

    std::array<float, kFftSize>   m_window;    // precomputed Hann
    std::vector<float>            m_smoothedDb; // size kBinCount
    bool                          m_primed{false};
};

} // namespace AetherSDR
