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

    // How many display frames the emitted spectrum integrates over (1 = one
    // un-averaged periodogram per frame, the default).
    //
    // Integration is in POWER, before the log. Averaging dB is the geometric mean:
    // it biases low on bursts (-100,-100,-100,-40 dBFS averages to -85 in dB vs -46
    // in power) and by a fixed -2.51 dB on a noise floor (RFC #5782 §3, #5794).
    // The estimator is an EMA with alpha = 1/frames: one vector of state, and every
    // frame still emits.
    //
    // Frames are DISPLAY frames, not consecutive periodograms: Hl2RxDsp calls
    // process() only when spectrumFrameDue(), so the time constant moves with the
    // fps cap (N = 8 at 25 fps spans ~320 ms).
    //
    // Nothing calls this yet. Wiring it to IRadioBackend::setPanAverage() (RFC
    // #5782) owes the 0..100 step-to-depth mapping, a meaning for
    // setPanWeightedAverage(), and engaging RadioCapabilities::backendPanAveraging
    // in the same change so SpectrumWidget's EMA does not stack on this one. The
    // signature is in frames so it is not mistaken for the operator's number.
    //
    // Changing the depth drops the accumulated state. Call on the DSP thread (the
    // hl2-io thread that calls process()): the members are unsynchronised, so use
    // a queued Q_INVOKABLE on Hl2RxDsp like setSpectrumRateFps. The class comment's
    // "construct off the real-time path" is about FFTW's planner, not this.
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

    // Drop the partial frame, returning how many samples went with it (0 = nothing
    // in flight). The caller is a TRANSPORT SEQUENCE GAP: process() carries a
    // partial frame across calls (~8 EP6 blocks of 126 samples per 1024-point
    // frame), so lost packets mid-frame would put a time discontinuity inside one
    // FFT. Discard rather than zero-fill, since a zero run renders as a broadband
    // transient. The return value separates "a gap arrived" from "a gap cost a
    // frame" (Hl2RxDsp::spectrumGapDiscards()).
    //
    // The averaging state is kept here: frames integrated before a gap still
    // measure the same spectrum. A geometry change must drop it; FFT size and IQ
    // rate do so for free because buildChannel() reconstructs this object.
    //
    // A pan RETUNE does not: setPanCenter()/setSliceFrequency() move the NCO
    // without reaching this object, so once setAverageFrames(N) is wired, dragging
    // the pan would blend bins integrated at the old NCO for ~N frames. Harmless
    // today (only hl2_spectrum_test averages). That wiring owes the retune a
    // distinct drop (a dropAverage() or a flag), not reset(): a gap and a retune
    // want opposite answers about the integrated frames.
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
