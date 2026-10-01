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

    // Display frames the spectrum integrates over (1 = none, the default), as a
    // power-domain EMA with alpha = 1/frames (averaging dB biases low, -2.51 dB
    // on noise; RFC #5782 §3, #5794). Display frames, so N = 8 at 25 fps ~320 ms.
    // Not yet wired to setPanAverage(); doing so must also set
    // RadioCapabilities::backendPanAveraging so SpectrumWidget's EMA doesn't
    // stack. Drops accumulated state. Call on the hl2-io thread (unsynchronised).
    void setAverageFrames(int frames) noexcept;
    [[nodiscard]] int averageFrames() const noexcept { return m_averageFrames; }

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
    // (An empty accumulator would cost ~9 EP6 blocks per frame, making frame
    // rate track the span.) Caps at fftSize - 1: process() fires on == fftSize
    // after a push_back, so a full buffer would step past it and never emit.
    void accumulate(std::span<const std::complex<float>> iq);

    // Drop the partial frame on a transport sequence gap, returning samples
    // discarded (0 = none): discard, not zero-fill, which renders as a transient.
    // Averaging state is kept (same spectrum). A pan retune moves the NCO
    // without reaching here, so wiring setAverageFrames() needs a separate drop
    // for retune; buildChannel() rebuilds this object on FFT size/rate change.
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
    // The square of it, precomputed. computeFrame() divides POWER by this
    // where it used to divide magnitude by m_coherentGain, and the two are the
    // same normalisation: (mag/g)^2 == (re^2 + im^2)/g^2.
    double m_coherentGainSq = 1.0;
    int m_averageFrames = 1;                  // 1 = no averaging (the default)
    // Per-bin EMA state in POWER, fftshifted the same way the emitted bins
    // are. Sized once at construction so setAverageFrames() and computeFrame()
    // never allocate — process() promises that in the header above.
    std::vector<double> m_avgPower;
    // Whether m_avgPower holds a frame yet. An EMA seeded at zero would show
    // the operator roughly `frames` frames of an artificially low floor every
    // time averaging is switched on or the geometry changes, so the first
    // frame after a drop is TAKEN rather than blended into silence.
    bool m_haveAverage = false;
    // Opaque FFTW handles (kept as void* so fftw3.h stays out of the header).
    void* m_in = nullptr;                     // fftw_complex[m_fftSize]
    void* m_out = nullptr;                     // fftw_complex[m_fftSize]
    void* m_plan = nullptr;                    // fftw_plan
};

}  // namespace AetherSDR::hl2
