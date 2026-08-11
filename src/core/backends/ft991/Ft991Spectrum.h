#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::ft991 {

// The FT-991 "panadapter" path: accumulate demodulated REAL audio into fixed
// FFT frames and keep the bins from 0 Hz to spanHz, in dBFS ascending audio
// frequency. A sibling of colibri::ColibriSpectrum (same Hanning window,
// per-frame DC removal, coherent-gain normalization) with the input real
// instead of complex and NO fftshift — audio has no negative frequencies,
// and the backend owns the audio->RF mapping (including the LSB reversal).
// Duplicated per-family per the backend layout rather than promoted.
//
// Owns an FFTW plan; construction/destruction allocate, process() does not.
// FFTW's global planner is not thread-safe, so construct off the sample path.
class Ft991Spectrum {
public:
    // sampleRateHz is the CAPTURE rate (the codec's native rate); spanHz is
    // how much of the audio band the frame keeps.
    Ft991Spectrum(int sampleRateHz, double spanHz, int fftSize = 8192);
    ~Ft991Spectrum();
    Ft991Spectrum(const Ft991Spectrum&) = delete;
    Ft991Spectrum& operator=(const Ft991Spectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }
    // Bins actually emitted per frame (0 Hz .. spanHz).
    [[nodiscard]] int binCount() const noexcept { return m_binCount; }
    [[nodiscard]] double binHz() const noexcept { return m_binHz; }
    // The span the emitted bins genuinely cover: binCount * binHz — what the
    // backend must advertise via panCenterBandwidthChanged, NOT the requested
    // spanHz (they differ by up to one bin's rounding).
    [[nodiscard]] double coveredSpanHz() const noexcept { return m_binCount * m_binHz; }

    // Append mono audio; each time a full frame accumulates, compute one
    // spectrum. `binsDbfs` is resized to binCount() (index 0 = 0 Hz,
    // ascending audio frequency). Returns frames produced; a partial frame
    // carries over.
    int process(std::span<const float> mono, std::vector<float>& binsDbfs);

    // Append WITHOUT transforming, keeping only the newest samples — the
    // display-rate cap's between-frames feed (same shape as ColibriSpectrum).
    void accumulate(std::span<const float> mono);

    void reset() noexcept { m_acc.clear(); }

private:
    void computeFrame(std::vector<float>& binsDbfs);

    int m_fftSize;
    int m_binCount;
    double m_binHz;
    std::vector<float> m_acc;
    std::vector<double> m_window;   // Hanning
    double m_coherentGain = 1.0;    // sum(window) / 2
    // Opaque FFTW handles (fftw3.h stays out of the header).
    void* m_in = nullptr;           // double[m_fftSize]
    void* m_out = nullptr;          // fftw_complex[m_fftSize/2 + 1]
    void* m_plan = nullptr;         // fftw_plan (r2c)
};

}  // namespace AetherSDR::ft991
