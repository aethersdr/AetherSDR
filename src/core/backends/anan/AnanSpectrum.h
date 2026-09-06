#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::anan {

// The ANAN-G2 panadapter path: accumulate raw IQ (normalized [-1, 1)) into
// fixed FFT frames and produce a DC-centered magnitude spectrum in dBFS.
// Duplicated from src/core/backends/hl2/Hl2Spectrum.h rather than shared —
// matches this codebase's established per-family convention (Icom's
// IcomScope is its own class too, not a shared spectrum decoder) — but the
// signal processing itself (Hanning window, per-frame DC removal, coherent-
// gain normalization, fftshift so DC lands at the centre bin) is a WDSP/FFT
// fact with no ANAN-specific content, so it is copied verbatim rather than
// re-derived.
//
// Owns an FFTW plan; construction/destruction allocate, process() does not
// (fftw_execute is allocation-free). FFTW's global planner is not
// thread-safe, so construct instances off the real-time path.
class AnanSpectrum {
public:
    explicit AnanSpectrum(int fftSize = 1024);
    ~AnanSpectrum();
    AnanSpectrum(const AnanSpectrum&) = delete;
    AnanSpectrum& operator=(const AnanSpectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }

    // Append IQ samples; each time a full frame accumulates, compute one
    // spectrum. `binsDbfs` is resized to fftSize (DC at index fftSize/2) and
    // holds the most recent frame. Returns the number of frames produced (a
    // partial frame is carried to the next call).
    int process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs);

    // Append IQ WITHOUT transforming, keeping only the newest samples. Used
    // while a display-rate cap is between frames, so the window keeps
    // filling instead of refilling from empty when the next frame comes due
    // — see Hl2Spectrum.h's own comment for the measured cost of not doing
    // this (achieved frame rate tracking the span instead of the operator's
    // fps slider).
    //
    // Caps at fftSize - 1 deliberately: a pre-filled buffer would step past
    // process()'s == fftSize frame-boundary check and never emit again.
    void accumulate(std::span<const std::complex<float>> iq);

    // Drop whatever partial frame has accumulated. Used on a geometry
    // change, where the samples either side genuinely describe different
    // windows.
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

}  // namespace AetherSDR::anan
