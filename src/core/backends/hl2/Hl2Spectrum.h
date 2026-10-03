#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
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
    // `sampleRateHz` is the IQ rate this object is fed at: it turns a count of
    // samples into time for setAverageTimeMs(). 0 (the default, for fixtures
    // that look at one frame) leaves time-based averaging inert.
    explicit Hl2Spectrum(int fftSize = 1024, double sampleRateHz = 0.0);
    ~Hl2Spectrum();
    Hl2Spectrum(const Hl2Spectrum&) = delete;
    Hl2Spectrum& operator=(const Hl2Spectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }

    // A fixed depth in display frames (1 = none): EMA with alpha = 1/frames, for
    // fixtures. The operator's FFT AVG is a time (setAverageTimeMs()); setting
    // this clears it. Drops the state. Call on the hl2-io thread (unsynchronised).
    void setAverageFrames(int frames) noexcept;
    [[nodiscard]] int averageFrames() const noexcept { return m_averageFrames; }

    // The operator's FFT AVG as a time constant in ms (0 = none). Each frame blends
    // with alpha = 1 - exp(-dt/tau), dt = IQ samples since the last frame / rate
    // (WDSP's analyzer law): response time is fps-invariant, noise reduction is not.
    // A transport gap does not advance this clock, so the pre-gap estimate holds.
    // A change drops the state. Inert without a sample rate.
    void setAverageTimeMs(double tauMs) noexcept;
    [[nodiscard]] double averageTimeMs() const noexcept { return m_averageTimeMs; }

    // The blend weight one frame gets after dtSeconds, at time constant
    // tauSeconds. Exposed so the mapping is testable without an FFT; tau <= 0
    // means no averaging and returns 1 (the new frame replaces the state).
    [[nodiscard]] static double blendAlpha(double dtSeconds, double tauSeconds) noexcept;

    // Averaging domain. false (default) integrates power and takes the log at
    // emit; true blends dBFS (log-recursive, WDSP average mode 3), ~2.5 dB lower
    // on noise. The operator's weighted-average toggle selects it, as on the
    // ANAN. A change drops the state.
    void setLogAverage(bool on) noexcept;
    [[nodiscard]] bool logAverage() const noexcept { return m_logAverage; }

    // Forget the running average and take the next frame whole. Keeps the
    // partial frame, which a setting change leaves valid; a retune (NCO move)
    // also calls reset() to discard it.
    void dropAverage() noexcept;

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
    // Averaging state is kept (same spectrum). A retune wants the opposite
    // answer and calls this AND dropAverage(); buildChannel() rebuilds this
    // object on FFT size/rate change.
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
    // Time-constant averaging; see setAverageTimeMs(). 0 = off. Takes
    // precedence over m_averageFrames, and each setter clears the other.
    double m_sampleRateHz = 0.0;
    double m_averageTimeMs = 0.0;
    bool m_logAverage = false;                // see setLogAverage()
    // IQ samples received since the last frame was computed — the dt of the
    // time-constant blend. Counts accumulate()'s discards too: they are time
    // that passed, whether or not they reach a transform.
    std::uint64_t m_samplesSinceFrame = 0;
    // Per-bin EMA state, fftshifted like the emitted bins; power, or dBFS while
    // m_logAverage is set. Sized once at construction so nothing here allocates.
    // HL2-private on purpose (RFC #5782 q3): it moves to src/core/dsp/ when a
    // second consumer (RTL) needs it.
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
