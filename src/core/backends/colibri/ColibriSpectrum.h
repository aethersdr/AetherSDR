#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::colibri {

// The Colibri panadapter path: accumulate raw IQ (normalized [-1, 1)) into
// fixed FFT frames and produce a DC-centered magnitude spectrum in dBFS.
// A sibling of hl2::Hl2Spectrum (same Hanning window, per-frame DC removal,
// coherent-gain normalization, fftshift) — duplicated under this family per
// the per-family backend layout rather than promoted, so neither backend's
// spectrum can be retuned out from under the other.
//
// Owns an FFTW plan; construction/destruction allocate, process() does not
// (fftw_execute is allocation-free). FFTW's global planner is not thread-safe,
// so construct instances off the real-time path.
class ColibriSpectrum {
public:
    explicit ColibriSpectrum(int fftSize = 1024);
    ~ColibriSpectrum();
    ColibriSpectrum(const ColibriSpectrum&) = delete;
    ColibriSpectrum& operator=(const ColibriSpectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }

    // Append IQ samples; each time a full frame accumulates, compute one
    // spectrum. `binsDbfs` is resized to fftSize (DC at index fftSize/2) and
    // holds the most recent frame. Returns the number of frames produced (a
    // partial frame is carried to the next call).
    int process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs);

    // Append IQ WITHOUT transforming, keeping only the newest samples — the
    // display-rate cap's between-frames feed. Caps at fftSize - 1 so a
    // pre-filled buffer cannot step past process()'s frame-boundary check.
    void accumulate(std::span<const std::complex<float>> iq);

    // Drop whatever partial frame has accumulated (geometry change).
    void reset() noexcept { m_acc.clear(); }

private:
    void computeFrame(std::vector<float>& binsDbfs);

    int m_fftSize;
    std::vector<std::complex<float>> m_acc;   // accumulation buffer (< m_fftSize)
    std::vector<double> m_window;             // Hanning window
    double m_coherentGain = 1.0;              // sum(window) / 2
    // Opaque FFTW handles (kept as void* so fftw3.h stays out of the header).
    void* m_in = nullptr;                     // fftw_complex[m_fftSize]
    void* m_out = nullptr;                    // fftw_complex[m_fftSize]
    void* m_plan = nullptr;                   // fftw_plan
};

}  // namespace AetherSDR::colibri
