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
    // `sampleRateHz` is the IQ rate this object is fed at. It is what turns a
    // count of samples into TIME for setAverageTimeMs() below; 0 (the default,
    // kept for the fixtures that only look at one frame) leaves time-based
    // averaging unavailable and every other behaviour unchanged.
    explicit Hl2Spectrum(int fftSize = 1024, double sampleRateHz = 0.0);
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
    // THE OPERATOR'S CONTROL DOES NOT COME THROUGH HERE. It arrives as a
    // TIME, through setAverageTimeMs() below, precisely because of the
    // paragraph above: a depth in frames would change its time constant every
    // time the fps slider moved. This frame-count form stays for fixtures that
    // want an exact 1/N, and setting it clears any averaging time.
    //
    // RFC #5782 q3, DECIDED in #5980: this accumulator is deliberately
    // HL2-private, not a shared helper in src/core/dsp/. It has one consumer —
    // the ANAN needs none, WDSP's analyzer averages there — and a shared
    // component with a single user is a guess at the second user's needs.
    // The RTL backend is that second user (RadioModel::requestPanAverage
    // still names it as not averaging); the change that wires it should MOVE
    // this into src/core/dsp/ and share it, not write it a third time.
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

    // Average over a TIME CONSTANT, in milliseconds; 0 = no averaging. This is
    // what the operator's FFT AVG reaches (Hl2Backend::setPanAverage, via
    // Hl2RxDsp::setSpectrumAverageMs), and it is the fix for the fps coupling
    // described above.
    //
    // Each emitted frame is blended with weight
    //
    //     alpha = 1 - exp(-dt / tau)
    //
    // where dt is the time since the PREVIOUS emitted frame, measured in IQ
    // SAMPLES divided by the sample rate — every sample handed to process() or
    // accumulate(), including those accumulate() then discards. The stream is
    // the clock, so the answer does not depend on the wall clock, the GUI
    // thread or scheduling jitter, and it is exact in a test.
    //
    // WHY THIS FORM. A continuous-time exponential with time constant tau,
    // sampled at arbitrary instants, is exactly this recursion; so the
    // estimator's memory is tau seconds whether frames arrive every 21 ms or
    // every 200 ms, and moving the fps slider mid-average needs no drop and no
    // re-derivation — the next frame's dt simply carries the new interval.
    // It is the same law WDSP's analyzer uses on the ANAN (AnanPanAnalyzer:
    // history weight exp(-1 / (fftsPerSecond * tau))), so one FFT AVG number
    // means one time constant on both host-averaged families.
    //
    // WHAT IT DOES NOT FIX. The RESPONSE time is fps-invariant; the amount of
    // NOISE REDUCTION is not, because only displayed frames are integrated
    // (see "FRAMES ARE DISPLAY FRAMES" above). Over one tau the average sees
    // tau / displayInterval independent periodograms, so a 500 ms average at
    // 25 fps integrates ~12 and at 5 fps ~2.5. Integrating every periodogram
    // instead would mean computing the FFTs the shaper exists to skip, which
    // is the cost decision Hl2RxDsp documents and not this class's to undo.
    //
    // A TRANSPORT GAP DOES NOT ADVANCE THIS CLOCK. reset() clears the partial
    // IQ frame but neither m_samplesSinceFrame nor the average, and lost EP6
    // samples are never counted. So after a dropout of any length the first
    // frame back is blended with dt ~ fftSize / fs (21 ms at 48 kHz), and the
    // pre-gap average is held, then decays over tau of STREAM time rather
    // than wall time. Deliberate: nothing is known about the signal during
    // the gap, and holding the last estimate is more honest than inventing a
    // decay toward a spectrum nobody measured.
    //
    // Changing the time DROPS the state, as a depth change does, and clears
    // any frame depth. Without a sample rate (constructed with 0) the setting
    // is stored but inert.
    void setAverageTimeMs(double tauMs) noexcept;
    [[nodiscard]] double averageTimeMs() const noexcept { return m_averageTimeMs; }

    // The blend weight one frame gets after dtSeconds, at time constant
    // tauSeconds. Exposed so the mapping is testable without an FFT; tau <= 0
    // means no averaging and returns 1 (the new frame replaces the state).
    [[nodiscard]] static double blendAlpha(double dtSeconds, double tauSeconds) noexcept;

    // WHICH DOMAIN the average runs in. false (the default) integrates POWER
    // and takes the log once at emit — the arithmetically correct estimator,
    // per the paragraph at the top of this block and #5794. true blends the
    // dBFS values instead: log-recursive, WDSP's average mode 3 and deskHPSDR's
    // default look, which sits ~2.5 dB lower on noise and crushes bursts, and
    // which some operators prefer precisely because it flattens the floor.
    //
    // This is what the operator's weighted-average toggle selects
    // (Hl2Backend::setPanWeightedAverage), with the same meaning it has on the
    // ANAN (AnanBackend::setPanWeightedAverage): on = log, off = power. The
    // widget's default is off, so the default on this radio is power.
    //
    // A change DROPS the state: a vector of powers is not a vector of dB.
    void setLogAverage(bool on) noexcept;
    [[nodiscard]] bool logAverage() const noexcept { return m_logAverage; }

    // Forget the running average and take the next frame whole. For a change
    // of the FREQUENCY AXIS (the NCO moved), which rebuilds nothing — see the
    // paragraph on reset() below for why that is a different answer from a
    // transport gap's. The partial IQ frame is left alone.
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
    // That retune drop is dropAverage() above, called by Hl2Backend wherever
    // it moves a receiver's NCO (setPanCenter, and setSliceFrequency when the
    // slice leaves the window). It is deliberately NOT this function: a
    // transport gap and a retune want OPPOSITE answers about the frames
    // already integrated, which is the whole reason this paragraph exists.
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
    // Per-bin EMA state, fftshifted the same way the emitted bins are: in
    // POWER, or in dBFS while m_logAverage is set. Sized once at construction so setAverageFrames() and computeFrame()
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
