#pragma once

#ifdef __APPLE__

#include <QByteArray>
#include <atomic>
#include <vector>
#include <Accelerate/Accelerate.h>

namespace AetherSDR {

// macOS spectral noise reduction using Apple Accelerate (vDSP).
//
// MMSE-Wiener filter with minimum-statistics noise floor tracking.
// Uses vDSP's real FFT, hardware-accelerated on Apple Silicon via AMX.
//
// Improvements over original (v0.7.9):
//   - Processes at 24 kHz natively — no 24↔48 kHz resampling.
//     Eliminates the double-resampler chain that caused clicks, phase
//     distortion, and int16 mid-point quantisation noise.
//   - 512-point FFT at 24 kHz → 46.9 Hz/bin (2× better than NR2).
//   - 25-frame noise history (~267 ms) vs 10 frames (~107 ms) before.
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
    void synthesizeFrameWithCurrentGain(const float* inBuf, float* outBuf);

    // ── FFT parameters ─────────────────────────────────────────────────
    static constexpr int LOG2N = 9;           // log2(512)
    static constexpr int N     = 1 << LOG2N;  // 512-point real FFT
    static constexpr int H     = N / 2;       // 256-sample hop (50 % overlap)
    static constexpr int NBINS = N / 2 + 1;   // 257 unique spectral bins

    // ── Algorithm tuning ───────────────────────────────────────────────
    static constexpr int   HIST    = 25;    // noise history frames (~267 ms)
    static constexpr float ALPHA   = 0.92f; // decision-directed smoothing
    static constexpr float OVER    = 2.0f;  // oversubtraction — punches harder at noise bins
    static constexpr float FLOOR   = 0.05f; // minimum Wiener gain (~26 dB max suppression)
    static constexpr float BIAS    = 1.2f;  // min-stats bias correction
    static constexpr float GSMOOTH = 0.70f; // temporal gain smoothing (faster response)

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
    float              m_powerHistory[HIST][NBINS]{};
    int                m_histIdx{0};
    std::vector<float> m_noiseEst;   // current noise floor estimate [NBINS]
    std::vector<float> m_prevGain;   // previous-frame Wiener gain [NBINS]
    std::vector<float> m_prevPow;    // previous-frame power spectrum [NBINS]
    std::vector<float> m_powerBuf;   // current power spectrum [NBINS]
    std::vector<float> m_gainBuf;    // raw Wiener gain per bin [NBINS]
    std::vector<float> m_smoothGain; // temporally smoothed gain [NBINS]
    int                m_frameCount{0};

    std::atomic<float> m_strength{1.0f};
};

} // namespace AetherSDR

#endif // __APPLE__
