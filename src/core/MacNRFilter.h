#pragma once

#ifdef __APPLE__

#include <QByteArray>
#include <algorithm>
#include <atomic>
#include <vector>
#include <Accelerate/Accelerate.h>

namespace AetherSDR {

// macOS spectral noise reduction using Apple Accelerate (vDSP).
//
// MMSE-Wiener filter with minimum-statistics noise floor tracking.
// Uses vDSP's real FFT, hardware-accelerated on Apple Silicon via AMX.
//
// Design characteristics:
//   - Processes at its immutable 24 or 48 kHz rate without resampling.
//     Eliminates the double-resampler chain that caused clicks, phase
//     distortion, and int16 mid-point quantisation noise.
//   - 512-point FFT at 24 kHz, 1024 at 48 kHz: 46.9 Hz/bin in both domains.
//   - Smoothed-periodogram minimum statistics with calibrated bias and
//     rate-limited upward tracking, using a 25-frame history (~267 ms).
//   - Near-silent frames freeze the learned noise floor so mute, squelch, and
//     TX gaps cannot collapse it and cause a burst when audio resumes.
//   - Per-bin Wiener gain is temporally smoothed (GSMOOTH) to suppress
//     musical-noise artefacts caused by rapid frame-to-frame gain swings.
//   - Output accumulator ensures exact byte-count match with no silence
//     gaps; the pre-filled FFT frame retains the same ≈21.3 ms delay at both rates.
//   - User-adjustable strength: 0 = bypass, 1 = full NR.
//
// Processing chain (entirely in the configured sample-rate domain):
//   stereo float32 -> shared mono FFT NR mask -> independent L/R OLA synthesis

class MacNRFilter {
public:
    explicit MacNRFilter(int sampleRate = 24000);
    ~MacNRFilter();

    bool isValid() const { return m_fftSetup != nullptr; }

    // Process configured-rate stereo float32 PCM; returns same format, same byte count.
    QByteArray process(const QByteArray& pcmStereo);

    int sampleRate() const { return m_sampleRate; }

    // Reset internal state (e.g. on band change or stream restart).
    void reset();

    // Noise-reduction strength: 0.0 = bypass, 1.0 = full suppression.
    // The underlying algorithm always runs at full strength; only the
    // blending into the output signal is scaled.
    // Thread-safe: AudioEngine sets this from the audio thread; the DSP
    // dialog reads/writes it from the UI thread.
    void  setStrength(float s) { m_strength.store(std::clamp(s, 0.0f, 1.0f)); }
    float strength()     const { return m_strength.load(); }

private:
    const int m_sampleRate;
    void updateGainFromFrame(const float* inBuf);
    void synthesizeFrameWithCurrentGain(const float* inBuf, float* outBuf,
                                        float synthesisStrength);

    // ── FFT parameters ─────────────────────────────────────────────────
    const int m_log2n;
    const int m_fftSize;
    const int m_hopSize;
    const int m_bins;

    // ── Algorithm tuning ───────────────────────────────────────────────
    static constexpr int   HIST             = 25;    // noise history frames (~267 ms)
    static constexpr float POWER_SMOOTH     = 0.80f;  // smoothed-periodogram coefficient
    static constexpr float MINSTAT_BIAS     = 1.85f;  // calibrated for a 25-frame smoothed minimum
    static constexpr float NOISE_RISE       = 0.90f;  // avoid chasing speech on upward noise updates
    static constexpr float MIN_FRAME_POWER  = 1e-8f;  // freeze estimator below -80 dBFS RMS
    static constexpr float INITIAL_NOISE_FRACTION = 0.25f; // avoid learning speech at enable time
    static constexpr float ALPHA            = 0.92f;  // decision-directed smoothing
    // Full strength deliberately drives stationary bins near FLOOR. The user
    // strength control blends this mask with dry audio for gentler reduction.
    static constexpr float OVER             = 2.0f;
    static constexpr float FLOOR            = 0.05f; // minimum Wiener gain (~26 dB max suppression)
    static constexpr float GSMOOTH          = 0.70f; // temporal gain smoothing (faster response)

    // ── vDSP state ─────────────────────────────────────────────────────
    FFTSetup           m_fftSetup{nullptr};
    std::vector<float> m_splitRe;   // split-complex real  [H]
    std::vector<float> m_splitIm;   // split-complex imag  [m_hopSize]

    // ── OLA buffers ────────────────────────────────────────────────────
    std::vector<float> m_window;    // sqrt-Hann analysis+synthesis window [m_fftSize]
    std::vector<float> m_inAccum;   // mono float input accumulator [m_fftSize]
    std::vector<float> m_inAccumL;  // left-channel input accumulator [m_fftSize]
    std::vector<float> m_inAccumR;  // right-channel input accumulator [m_fftSize]
    std::vector<float> m_olaBufferL; // left overlap-add accumulator [m_fftSize]
    std::vector<float> m_olaBufferR; // right overlap-add accumulator [m_fftSize]
    std::vector<float> m_frameBuf;  // windowed analysis frame [m_fftSize]
    std::vector<float> m_synthBuf;  // synthesis frame [m_fftSize]
    std::vector<float> m_outAccumL; // processed left-channel output
    std::vector<float> m_outAccumR; // processed right-channel output

    // ── Noise estimator state ─────────────────────────────────────────
    std::vector<float> m_powerHistory; // smoothed periodograms, HIST rows of m_bins
    int                m_histIdx{0};
    std::vector<float> m_noiseEst;       // current noise floor estimate [m_bins]
    std::vector<float> m_smoothedPower;  // current smoothed periodogram [m_bins]
    std::vector<float> m_prevPostSnr;    // previous a-posteriori SNR [m_bins]
    std::vector<float> m_filterGain;     // unblended synthesis mask [m_bins]
    std::vector<float> m_powerBuf;       // current power spectrum [m_bins]
    std::vector<float> m_gainBuf;        // raw Wiener gain per bin [m_bins]
    bool               m_noiseInitialized{false};

    std::atomic<float> m_strength{1.0f};
};

} // namespace AetherSDR

#endif // __APPLE__
