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
// THE LAST STEP DIVERGED IN #5833 and the divergence is intended. Hl2Spectrum
// now squares in place and emits 10*log10(power + 1e-24) where this class
// still takes the square root and emits 20*log10(mag + 1e-12). The four items
// listed above are untouched and "verbatim" still describes them; only the
// conversion to dBFS differs, and for a SINGLE frame the two are the same
// number by the same route, since 20*log10(m) is 10*log10(m^2).
//
// WHY HL2 MOVED AND THIS DID NOT. Hl2Spectrum integrates across frames, and an
// arithmetic mean of logarithms is the logarithm of the GEOMETRIC mean, which
// biases low exactly where a panadapter is noisiest (#5794; derivation in RFC
// #5782 §3). Accumulating in POWER is the fix, and taking the log once at emit
// is what makes it possible. This class does not average, so it gains nothing
// from the change and is deliberately left alone: an HL2 bring-up PR is not
// the place to move an ANAN signal path (docs/HERMES.md, "keep bring-up inside
// the family backend"; AGENTS.md's #5554 notice, "no copy of HL2 scaffolding
// into another host-DSP family").
//
// Nor would mirroring be a free no-op. The epsilon is not a translation: the
// old floor puts an exactly-zero bin at -120 dBFS and the new one at -240, so
// copying the arithmetic across would change what this class emits for a
// silent bin, and nothing here has measured what ANAN's consumers do with
// -240. If this class ever grows averaging, move it then and re-derive the
// floor with it.
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

    // Drop whatever partial frame has accumulated, returning how many samples
    // went with it (0 = nothing was in flight).
    //
    // Identical in purpose and in reasoning to Hl2Spectrum::reset(), which
    // carries the full note: the geometry change this was written for
    // reconstructs the object anyway (AnanRxDsp::installChannel() moves a freshly
    // built AnanSpectrum in), so the real caller is a DDC sequence gap, where the
    // samples either side of the seam describe different instants and an FFT
    // across them measures nothing.
    std::size_t reset() noexcept
    {
        const std::size_t discarded = m_acc.size();
        m_acc.clear();
        return discarded;
    }

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
