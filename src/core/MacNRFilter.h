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
//   - Processes at 24 kHz natively — no 24↔48 kHz resampling.
//     Eliminates the double-resampler chain that caused clicks, phase
//     distortion, and int16 mid-point quantisation noise.
//   - 512-point FFT at 24 kHz → 46.9 Hz/bin (2× better than NR2).
//   - Smoothed-periodogram minimum statistics with calibrated bias and
//     rate-limited upward tracking, using a 25-frame history (~267 ms).
//   - Near-silent frames freeze the learned noise floor so mute, squelch, and
//     TX gaps cannot collapse it and cause a burst when audio resumes.
//   - Per-bin Wiener gain is temporally smoothed (GSMOOTH) to suppress
//     musical-noise artefacts caused by rapid frame-to-frame gain swings.
//   - Output accumulator ensures exact byte-count match with no silence
//     gaps; startup latency is one hop (≈10.7 ms) of pre-filled zeros.
//   - User-adjustable strength: 0 = bypass, 1 = full NR.
//
// Processing chain (all at 24 kHz):
//   stereo float32 -> shared mono FFT NR mask -> independent L/R OLA synthesis

class MacNRFilter {
public:
    MacNRFilter();
    ~MacNRFilter();

    bool isValid() const { return m_fftSetup != nullptr; }

    // Process 24 kHz stereo float32 PCM; returns same format, same byte count.
    QByteArray process(const QByteArray& pcm24kStereo);

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
    void updateGainFromFrame(const float* inBuf);
    void synthesizeFrameWithCurrentGain(const float* inBuf, float* outBuf,
                                        float synthesisStrength);

    // ── FFT parameters ─────────────────────────────────────────────────
    static constexpr int LOG2N = 9;           // log2(512)
    static constexpr int N     = 1 << LOG2N;  // 512-point real FFT
    static constexpr int H     = N / 2;       // 256-sample hop (50 % overlap)
    static constexpr int NBINS = N / 2 + 1;   // 257 unique spectral bins

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
    std::vector<float> m_splitIm;   // split-complex imag  [H]

    // ── OLA buffers ────────────────────────────────────────────────────
    std::vector<float> m_window;    // sqrt-Hann analysis+synthesis window [N]
    std::vector<float> m_inAccum;   // 24 kHz mono float input accumulator
    std::vector<float> m_inAccumL;  // 24 kHz left-channel input accumulator
    std::vector<float> m_inAccumR;  // 24 kHz right-channel input accumulator
    std::vector<float> m_olaBufferL; // left overlap-add accumulator [N]
    std::vector<float> m_olaBufferR; // right overlap-add accumulator [N]
    std::vector<float> m_frameBuf;  // windowed analysis frame [N]
    std::vector<float> m_synthBuf;  // synthesis frame [N]
    std::vector<float> m_outAccumL; // processed 24 kHz left-channel output
    std::vector<float> m_outAccumR; // processed 24 kHz right-channel output

    // ── Noise estimator state ─────────────────────────────────────────
    float              m_powerHistory[HIST][NBINS]{}; // smoothed periodograms
    int                m_histIdx{0};
    std::vector<float> m_noiseEst;       // current noise floor estimate [NBINS]
    std::vector<float> m_smoothedPower;  // current smoothed periodogram [NBINS]
    std::vector<float> m_prevPostSnr;    // previous a-posteriori SNR [NBINS]
    std::vector<float> m_filterGain;     // unblended synthesis mask [NBINS]
    std::vector<float> m_powerBuf;       // current power spectrum [NBINS]
    std::vector<float> m_gainBuf;        // raw Wiener gain per bin [NBINS]
    bool               m_noiseInitialized{false};

    std::atomic<float> m_strength{1.0f};
};

} // namespace AetherSDR

#endif // __APPLE__
