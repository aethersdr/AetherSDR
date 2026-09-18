#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {

// The HL2 panadapter path: accumulate raw IQ (normalized [-1, 1)) into fixed
// FFT frames and produce a DC-centered magnitude spectrum in dBFS. Ported from
// the live-validated tools/hl2/spectrum.py — Hanning-windowed, per-frame DC
// removal (the direct-sampling ADC offset sits on I), coherent-gain normalized,
// fftshifted so DC lands at the centre bin.
//
// Owns an FFTW plan; construction/destruction allocate, process() does not
// (fftw_execute is allocation-free). FFTW's global planner is not thread-safe,
// so construct instances off the real-time path (single-channel today).
class Hl2Spectrum {
public:
    explicit Hl2Spectrum(int fftSize = 1024);
    ~Hl2Spectrum();
    Hl2Spectrum(const Hl2Spectrum&) = delete;
    Hl2Spectrum& operator=(const Hl2Spectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }

    // Append IQ samples; each time a full frame accumulates, compute one
    // spectrum. `binsDbfs` is resized to fftSize (DC at index fftSize/2) and
    // holds the most recent frame. Returns the number of frames produced (a
    // partial frame is carried to the next call).
    int process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs);

    // Append IQ WITHOUT transforming, keeping only the newest samples. Used
    // while the display-rate cap is between frames: the window keeps filling, so
    // when the next frame comes due it completes from recent contiguous samples
    // instead of refilling from empty.
    //
    // Refilling was what made the achieved frame rate track the SPAN rather than
    // the operator's slider. A frame is fftSize samples and an EP6 block is 126,
    // so an empty accumulator costs ~9 block intervals before a frame can be
    // emitted at all — 23.6 ms at 48 kHz but only 3.0 ms at 384 kHz. Feeding it
    // instead bounds that to a single block.
    //
    // Caps at fftSize - 1 deliberately: older samples can never contribute to
    // the next transform, and leaving the buffer exactly full would break
    // process()'s frame-boundary detection (it fires on == fftSize after a
    // push_back, so a pre-filled buffer would step straight past it and never
    // emit another frame).
    void accumulate(std::span<const std::complex<float>> iq);

    // Drop whatever partial frame has accumulated, returning how many samples
    // went with it (0 = nothing was in flight, so nothing was salvaged).
    //
    // The caller is a TRANSPORT SEQUENCE GAP, not the geometry change this was
    // originally written for. A geometry change RECONSTRUCTS this object --
    // Hl2RxDsp::configure() does `m_spectrum = std::make_unique<Hl2Spectrum>(...)`
    // -- so the accumulator is already empty on the far side of one, and that is
    // why this function sat with no caller in the tree at all. The case that
    // genuinely needs it is the one nobody wired: lost EP6 packets.
    //
    // WHY A GAP MATTERS HERE and not merely to a packet counter. process()
    // carries a partial frame ACROSS calls and transforms only on
    // `m_acc.size() == m_fftSize`. An EP6 block is 126 IQ samples and a frame is
    // fftSize, so ~8 blocks build one frame at the 1024 points every backend
    // here actually runs. When packets are lost mid-frame the accumulator keeps
    // the pre-gap samples and fills the rest from post-gap ones: the FFT then
    // spans a time discontinuity, and the phase relationship across the seam is
    // not a measurement of anything. Discarding is the right answer rather than
    // zero-filling the hole -- there is no sample to interpolate, the radio
    // never sent it, and a zero run is a broadband transient this window would
    // faithfully render as signal.
    //
    // The RETURN VALUE is what makes "a gap arrived" distinguishable from "a gap
    // cost us a frame". A gap landing exactly on a frame boundary discards
    // nothing and corrupts nothing; see Hl2RxDsp::spectrumGapDiscards().
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
    void* m_out = nullptr;                     // fftw_complex[m_fftSize]
    void* m_plan = nullptr;                    // fftw_plan
};

}  // namespace AetherSDR::hl2
