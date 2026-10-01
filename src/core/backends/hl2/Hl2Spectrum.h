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

    // How many frames the emitted spectrum integrates over. 1 (the default) is
    // one un-averaged periodogram per frame — the behaviour this class has
    // always had, and the behaviour it keeps until something sets this.
    //
    // THE INTEGRATION HAPPENS IN POWER, BEFORE THE LOG, and that is the whole
    // point rather than an implementation choice. The arithmetic mean of
    // logarithms is the logarithm of the GEOMETRIC mean, which sits below the
    // arithmetic mean and biases low exactly where the display is noisiest.
    // One bin, four frames, a burst in the last of them (-100, -100, -100,
    // -40 dBFS): the power average is -46.0 dBFS and the dB average is -85.0,
    // so 39 dB of a 60 dB event is thrown away, and it gets worse the harder
    // the operator averages. A noise floor is biased too — per-bin power is
    // exponentially distributed and E[ln X] = ln(mean) - gamma, a fixed
    // -2.51 dB that never integrates away. A steady unmodulated carrier is the
    // one case where the two agree, which is why nobody has reported it.
    // (#5794; the full derivation is in RFC #5782 §3.)
    //
    // The estimator is an EMA with alpha = 1/frames, not an N-frame boxcar.
    // #5794 leaves that choice open and states this as its default: it is one
    // vector of state rather than a ring of N x fftSize doubles (512 kB at
    // N=16, fftSize=4096), and it does not divide the display cadence by N —
    // every frame still emits, which is not the operator's to lose.
    //
    // "FRAMES" ARE DISPLAY FRAMES, NOT CONSECUTIVE PERIODOGRAMS, so the
    // integration window moves with the operator's fps slider.
    // Hl2RxDsp::processIqBlock() calls process() only when its
    // spectrumFrameDue() says so; every other EP6 block goes to accumulate(),
    // which keeps at most fftSize - 1 samples and discards the rest. One
    // periodogram per display interval is therefore all this ever integrates:
    // N = 8 at 25 fps spans ~320 ms of wall clock sampled once every 40 ms,
    // not 8 back-to-back transforms. Moving the fps cap changes the time
    // constant without changing this number — a constraint on whatever
    // step-to-depth mapping the setPanAverage() wiring chooses, not a defect
    // here.
    //
    // NOTHING CALLS THIS YET, DELIBERATELY. RFC #5782 has since ruled the seam
    // (q2): IRadioBackend::setPanAverage() carries the operator's 0..100 FFT
    // AVG, and what one step means is the backend's call. Wiring HL2 to it is
    // that PR's work, and it owes three things: the step-to-depth mapping;
    // what setPanWeightedAverage() means here (on ANAN it selects WDSP's
    // log-recursive mode, so HL2 either offers a dB-domain blend behind it or
    // says it ignores it); and engaging RadioCapabilities::backendPanAveraging
    // IN THE SAME CHANGE, so SpectrumWidget's own EMA does not stack on this
    // one. Whether this accumulator moves to src/core/dsp/ (q3) is that PR's
    // call too. This signature is in FRAMES precisely so it is not mistaken
    // for the operator's number.
    //
    // Changing the depth DROPS the accumulated state: an exponential state
    // built at one alpha does not mean anything at another, and carrying it
    // would make the first frames after a change describe a blend of two
    // estimators.
    //
    // CALL IT ON THE DSP THREAD — the same thread that calls process(), which
    // for the production owner is Hl2RxDsp's, the hl2-io thread Hl2Backend
    // moves it to. m_averageFrames, m_avgPower and m_haveAverage are plain
    // members with no atomic and no lock, and computeFrame() reads all three,
    // so a call from any other thread is a data race; AGENTS.md's "atomic
    // parameters for cross-thread DSP" rule is met by none of them. The
    // established route is the one every other setter on that chain already
    // takes — Q_INVOKABLE on Hl2RxDsp, reached with
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection); see
    // Hl2RxDsp::setSpectrumRateFps and Hl2Backend::pushNoiseBlanker. The work
    // is cheap enough for that thread: a vector assign, not an allocation,
    // because the state is sized at construction.
    //
    // (The class comment above says to CONSTRUCT instances off the real-time
    // path. That is about FFTW's process-global, non-thread-safe planner and
    // applies to the constructor alone — it is not licence to reach into a
    // live instance from another thread.)
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
    //
    // THE AVERAGING STATE IS NOT DROPPED HERE, and that is a decision rather
    // than an omission. What this function handles is a transport gap, and the
    // frames integrated BEFORE a gap are still measurements of the same
    // spectrum; throwing them away on every burst of packet loss would make
    // the display oscillate between averaged and raw. Only the partial frame,
    // whose samples would span the discontinuity, is discarded.
    //
    // A GEOMETRY CHANGE MUST drop the average, and two thirds of geometry drop
    // it for free. FFT size and IQ rate both live in Hl2RxDsp::Config, both
    // reach Hl2RxDsp::buildChannel(), and buildChannel() does
    // make_unique<Hl2Spectrum>(config.fftSize) — a fresh object, m_avgPower
    // zeroed and m_haveAverage false. That is the reconstruction the paragraph
    // above describes.
    //
    // THE FREQUENCY AXIS IS GEOMETRY TOO, AND IT HAS NO SUCH PATH. A pan
    // retune is not in Config and rebuilds nothing:
    // Hl2Backend::setPanCenter() and Hl2Backend::setSliceFrequency() move the
    // NCO with invokeMethod(m_metis, "setRxFrequencyHz", ...) and re-push the
    // WDSP shift, and neither reaches this object or this function. So once
    // anything calls setAverageFrames(N), dragging the pan blends the new
    // spectrum into bins integrated at the OLD NCO, and the EMA carries them
    // for roughly N display frames — at 25 fps and N = 16, ghosts at wrong
    // frequencies for the better part of a second. #5794's own constraint,
    // that state must be dropped across a geometry change, covers this axis.
    //
    // Harmless TODAY only because nothing on the shipping path calls
    // setAverageFrames() — only hl2_spectrum_test does.
    // Whoever wires RFC #5782's operator control owes the retune a DISTINCT
    // drop — a dropAverage(), or a flag on this function — and must not simply
    // call reset() from it: a transport gap and a retune want OPPOSITE answers
    // about the frames already integrated, which is the whole reason this
    // paragraph exists. Deliberately not implemented here; it is unreachable
    // until that wiring lands, and the hook belongs with its caller.
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
