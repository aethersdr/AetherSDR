#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AetherSDR::anan {

// The ANAN-G2 panadapter spectrum, computed by WDSP's display analyzer
// (third_party/wdsp/upstream/analyzer.c) rather than a hand-rolled FFT.
//
// The analyzer is the engine the openHPSDR desktop clients drive for this
// radio family, and the settings below follow deskHPSDR's defaults:
//
//   - FFT size: the next power of two at or above the output point count,
//     never below 16384. At 192 ksps that is ~12 Hz per bin, so each output
//     point averages many bins.
//   - Kaiser window (PiAlpha 14).
//   - Per-point AVERAGE detector: each output point is the mean of the FFT
//     bins it covers. This, not time averaging, is what takes the grain out
//     of a single FFT: at 1024 points from a 16k FFT every point averages 16
//     bins.
//   - Overlap sized so that one full-length FFT completes per display frame,
//     sliding over fresh samples, so no sample is thrown away between frames.
//   - Time averaging set by the operator, as an averaging TIME in ms -- the
//     control deskHPSDR exposes: 0 = none (each frame is one FFT), t > 0 =
//     log-recursive (WDSP average mode 3, deskHPSDR's default mode) weighting
//     history by exp(-1 / (F * t)) with F the analyzer's FFT rate -- fps
//     while one FFT fits per display frame, more once the overlap saturates
//     (see derive()). deskHPSDR's default is 250 ms. The
//     operator can switch the recursive mode to linear (an average of power)
//     as deskHPSDR's averaging-mode choice allows.
//   - Levels normalised to the bandwidth of one output point.
//
// One addition of our own: the running average is SEEDED from the first frame
// -- after creation, and again whenever averaging is switched back on --
// instead of rising from WDSP's -160 dB starting value. AetherSDR rebuilds the
// analyzer on every DDC rate change (every zoom), so an unseeded average would
// fade the panadapter in from black after each one, and a droop calibration
// sweep's post-rate-change settle would measure the ramp.
//
// Input is RAW WIRE IQ, not a conjugate: Spectrum0() swaps I and Q, which
// mirrors the spectrum, and that swap is what turns the HPSDR wire's
// convention into the right way round -- the same arrangement the desktop
// clients use. Output points run from the lowest frequency to the highest.
//
// Threading. create() plans FFTs (FFTW_PATIENT) and must run off any
// real-time thread; it takes WdspChannel::fftwSetupLock() itself. Everything
// else -- feed(), takeFrame(), setFramesPerSecond(), setNumPoints(), the
// destructor -- must run on ONE thread, the one that feeds samples:
// SetAnalyzer() resets the input ring and cannot race Spectrum0(). The FFTs
// themselves run on WDSP-owned worker threads.
class AnanPanAnalyzer {
public:
    struct Settings {
        int sampleRateHz = 48000;
        int numPoints = 1024;          // output points per frame
        int framesPerSecond = 25;      // display frame rate
        int averageTimeMs = 0;         // time average, ms; 0 = none
        bool logAverage = true;        // log-recursive (true) or linear-recursive
    };

    // Complex samples handed to the analyzer per Spectrum0() call. A power of
    // two, so it divides the analyzer's input ring (2 * kMaxFftSize).
    static constexpr int kBlockSize = 1024;
    // Largest FFT any setting can ask for, and the analyzer's buffer size.
    // XCreateAnalyzer sizes its buffers for this and for WDSP's own maxima
    // (dMAX_PIXOUTS x dMAX_AVERAGE x dMAX_PIXELS window-average frames among
    // them): about 45 MB per analyzer, and two exist briefly during a
    // rate-change rebuild.
    static constexpr int kMaxFftSize = 65536;
    static constexpr int kMinFftSize = 16384;
    // Most output points the analyzer can return (WDSP's dMAX_PIXELS).
    static constexpr int kMaxPoints = 16384;

    // Builds and configures the analyzer in slot `disp` (0..71). Returns
    // nullptr and sets `error` if the slot cannot be created.
    [[nodiscard]] static std::unique_ptr<AnanPanAnalyzer> create(int disp, const Settings& settings,
                                                                std::string* error = nullptr);
    ~AnanPanAnalyzer();
    AnanPanAnalyzer(const AnanPanAnalyzer&) = delete;
    AnanPanAnalyzer& operator=(const AnanPanAnalyzer&) = delete;

    [[nodiscard]] int disp() const noexcept { return m_disp; }
    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }
    [[nodiscard]] int numPoints() const noexcept { return m_settings.numPoints; }
    [[nodiscard]] int framesPerSecond() const noexcept { return m_settings.framesPerSecond; }

    // The frame-rate-dependent settings -- overlap and the averaging weights --
    // re-applied for a new display rate. The FFT size does not change, so
    // nothing is re-planned; the running average is kept.
    void setFramesPerSecond(int fps);

    // The output point count, for a new panel width. Clamped to
    // 2..kMaxPoints. The FFT size cannot change -- every count up to
    // kMaxPoints fits in kMinFftSize -- so nothing is re-planned, but the
    // running average is per point, so the next frame re-seeds it.
    void setNumPoints(int points);

    // The time average, in ms: 0 = none, t > 0 = log-recursive with time
    // constant t. Cheap -- no re-plan, no ring reset. Switching averaging back
    // on re-seeds from the next frame.
    void setAverageTimeMs(int ms);

    // Log-recursive (true, deskHPSDR's default) or linear-recursive (false)
    // averaging. Cheap; re-seeds, since entering either mode resets WDSP's
    // history.
    void setLogAverage(bool on);

    // Stage raw wire IQ and hand the analyzer every whole kBlockSize block.
    // A partial block is carried to the next call.
    void feed(std::span<const std::complex<float>> iq);

    // Drop a partially staged block, returning how many samples went with it
    // (0 = nothing was in flight). The analyzer's own history is left alone.
    std::size_t dropStagedPartial() noexcept;

    // If the analyzer has produced a frame since the last call, copy it into
    // `pointsDb` (resized to numPoints(), dB, lowest frequency first) and
    // return true. Returns false, leaving `pointsDb` untouched, otherwise.
    bool takeFrame(std::vector<float>& pointsDb);

    // The derived analyzer parameters for a given setting -- exposed so tests
    // can pin the formulas without a running analyzer.
    struct Derived {
        int fftSize = 0;
        int overlap = 0;
        int maxWriteahead = 0;
        double avBackmult = 0.0;
        int numAverage = 0;
    };
    [[nodiscard]] static Derived derive(const Settings& settings) noexcept;

private:
    AnanPanAnalyzer(int disp, const Settings& settings);
    void applySettings();

    int m_disp;
    Settings m_settings;
    int m_fftSize = 0;
    double m_avBackmult = 0.0;
    void applyAveraging();
    bool m_seeded = false;
    std::vector<double> m_staged;          // interleaved I,Q -- 2 * kBlockSize
    std::size_t m_stagedCount = 0;         // complex samples in m_staged
    std::vector<float> m_scratch;          // GetPixels target, numPoints
};

}  // namespace AetherSDR::anan
