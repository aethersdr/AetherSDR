/*  SpectralNR.h

This file is part of AetherSDR.

Portions of this file are derived from WDSP (emnr.c):
  Copyright (C) 2015, 2025 Warren Pratt, NR0V
  https://github.com/TAPR/OpenHPSDR-wdsp

The WDSP-derived portions are licensed under the GNU General Public License
as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

AetherSDR integration and C++20/Qt6 adaptation:
  Copyright (C) 2024-2026 AetherSDR Contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_FFTW3
#include <fftw3.h>
#endif

namespace AetherSDR {

// Client-side spectral noise reduction using the WDSP Gaussian/Gamma speech
// estimators with WDSP OSMS, MMSE, or non-stationary noise-floor tracking.
// Derived from WDSP NR2 (emnr.c) by Warren Pratt, NR0V.
//
// Uses FFTW3 for FFT computation (with wisdom file for optimised plans)
// when available; falls back to a built-in radix-2 FFT otherwise.
//
// Processes mono float32 audio at 24 kHz.  For stereo Flex speaker audio,
// processStereoSharedMask() computes one mono NR mask and applies it to both
// original channels so radio-side balance survives client NR.

class SpectralNR {
public:
    explicit SpectralNR(int fftSize = 256, int sampleRate = 24000,
                        int overlap = 2,
                        bool useLegacyGainMethods = false);
    ~SpectralNR();

    SpectralNR(const SpectralNR&) = delete;
    SpectralNR& operator=(const SpectralNR&) = delete;

    // Feed mono float32 samples in, get noise-reduced mono float32 out.
    // Output buffer must be at least numSamples long.
    void process(const float* input, float* output, int numSamples);

    // Feed interleaved stereo float32 samples in, get interleaved stereo
    // float32 out.  The NR estimate/mask is computed from (L+R)/2, then the
    // same spectral gain is applied to each original channel.
    // Output buffer must be at least numFrames * 2 samples long.
    // Use only one process entry point for an instance between resets; the mono
    // and stereo paths share ring cursors but maintain different OLA buffers.
    void processStereoSharedMask(const float* input, float* output, int numFrames);

    // Reset all internal state (call when toggling on or stream restarts).
    void reset();

    // User-adjustable parameters (thread-safe, called from main thread)
    void setGainMax(float v);
    void setGainFloor(float v);
    void setQspp(float v);
    void setGainSmooth(float v);
    float gainMax() const       { return static_cast<float>(m_gainMax.load()); }
    float gainFloor() const     { return static_cast<float>(m_gainFloor.load()); }
    float qspp() const         { return static_cast<float>(m_qSpp.load()); }
    float gainSmooth() const    { return static_cast<float>(m_gainSmooth.load()); }
    // Gain method: 0=Linear, 1=Log, 2=Gamma (default, MMSE-LSA), 3=Trained
    void setGainMethod(int m);
    int  gainMethod() const     { return m_gainMethod.load(); }

    // NPE method: 0=OSMS (default), 1=MMSE, 2=NSTAT
    void setNpeMethod(int m);
    int  npeMethod() const      { return m_npeMethod.load(); }

    // AE filter: artifact elimination post-processing
    void setAeFilter(bool on)   { m_aeFilter.store(on); }
    bool aeFilter() const       { return m_aeFilter.load(); }

    int fftSize() const { return m_fftSize; }
    bool usesLegacyGainMethods() const { return m_useLegacyGainMethods; }
#ifdef HAVE_FFTW3
    bool hasPlanFailed() const { return m_planFailed; }
#else
    bool hasPlanFailed() const { return false; }
#endif

    // Generate FFTW wisdom file for optimal FFT performance.
    // Call once on first use; subsequent runs load existing wisdom.
    // The progress callback receives (currentStep, totalSteps, description).
    using WisdomProgressCb = std::function<void(int, int, const std::string&)>;
    using WisdomCancelCb = std::function<bool()>;
    enum class WisdomResult {
        Ready,      // existing or imported wisdom is ready
        Generated,  // new wisdom was generated and saved
        Cancelled,  // caller cancelled before wisdom was ready
        Failed,     // generation or export failed
    };
    static bool loadWisdom(const std::string& directory);
    static WisdomResult generateWisdom(const std::string& directory,
                                       WisdomProgressCb progress = nullptr,
                                       WisdomCancelCb shouldCancel = nullptr);

private:
    // FFTW plan creation/destruction is NOT thread-safe. This mutex guards
    // all fftw_plan_*, fftw_destroy_plan, and wisdom import/export calls.
    // fftw_execute() is thread-safe and does not need the lock. (#467)
    static std::mutex s_fftwMutex;
    // FFT parameters
    int m_fftSize;
    int m_overlap;          // supported values: 2 (50%) or 4 (75%)
    int m_hopSize;          // fftSize / overlap
    int m_msize;            // fftSize / 2 + 1  (real-FFT bin count)
    int m_sampleRate;
    double m_olaScale;      // unity-gain normalization for periodic Hann COLA
    bool m_useLegacyGainMethods{false};

    // Overlap-add accumulators
    std::vector<double> m_inAccum;      // circular input buffer
    int m_inWritePos{0};
    int m_inReadPos{0};
    int m_samplesAccum{0};

    std::vector<double> m_outAccum;     // overlap-add output ring
    int m_outWritePos{0};
    int m_outReadPos{0};
    int m_outputAvailable{0};           // finalized samples queued for callers
    std::vector<double> m_stereoInAccumL;
    std::vector<double> m_stereoInAccumR;
    std::vector<double> m_stereoOutAccumL;
    std::vector<double> m_stereoOutAccumR;

    // Window
    std::vector<double> m_window;

    // FFT working buffers
    std::vector<double> m_fftIn;        // time-domain input (windowed)
    std::vector<double> m_ifftOut;      // inverse FFT result

#ifdef HAVE_FFTW3
    fftw_complex* m_fftOut{nullptr};    // forward FFT output (FFTW-allocated)
    fftw_complex* m_ifftIn{nullptr};    // inverse FFT input  (FFTW-allocated)
    fftw_plan     m_planFwd{nullptr};
    fftw_plan     m_planRev{nullptr};
    bool          m_planFailed{false};
#else
    // Fallback: built-in radix-2 FFT scratch buffers
    std::vector<double> m_fftScratchRe;
    std::vector<double> m_fftScratchIm;
    std::vector<double> m_fftScratchRe2;
    std::vector<double> m_fftScratchIm2;
    std::vector<int>    m_bitRev;

    void initBitReversal();
    void fftForward(const double* timeIn, double* re, double* im);
    void fftInverse(const double* re, const double* im, double* timeOut);
#endif

    // Frequency-domain bins (real/imag separate, msize elements)
    std::vector<double> m_freqRe;
    std::vector<double> m_freqIm;
    std::vector<double> m_gainRe;       // gain-applied freq bins
    std::vector<double> m_gainIm;

    // Selected noise estimate consumed by the gain stage. Each NPE method has
    // its own history below. All three histories stay warm on every frame so
    // a live method switch cannot expose a reset-time estimate.
    std::vector<double> m_noisePsd;     // lambda_d -- selected noise PSD

    // Noise estimation (OSMS) per-bin state
    std::vector<double> m_osmsNoisePsd; // sigma2N -- OSMS noise PSD
    std::vector<double> m_smoothPsd;    // p(k)      -- smoothed periodogram
    std::vector<double> m_pMin;         // running minimum per bin
    std::vector<double> m_pBar;         // variance estimator: mean of p
    std::vector<double> m_p2Bar;        // variance estimator: mean of p^2
    std::vector<double> m_alphaOpt;     // per-bin optimal smoothing factor
    std::vector<double> m_alphaHat;     // per-bin effective smoothing factor
    double m_alphaC{1.0};               // global correction factor

    // OSMS sub-window tracking
    std::vector<double> m_actMin;       // current sub-window minimum
    std::vector<double> m_actMinSub;    // sub-frame minimum
    std::vector<std::vector<double>> m_actMinBuf; // circular buffer of U sub-windows
    std::vector<int> m_kMod;            // current frame found a new sub-window minimum
    std::vector<int> m_lminFlag;
    int m_subwc{1};                     // sub-window counter
    int m_ambIdx{0};                    // circular index into actMinBuf
    int m_U{8};                         // number of sub-windows
    int m_V{15};                        // frames per sub-window
    int m_D;                            // U * V
    double m_alphaMax{0.96};            // hop-scaled OSMS maximum smoothing
    double m_alphaCMin{0.7};            // hop-scaled global correction floor
    double m_alphaMinMax{0.3};          // hop-scaled optimal-smoothing floor cap
    double m_snrq{-0.25};               // hop-scaled SNR exponent
    double m_betaMax{0.8};              // hop-scaled variance smoothing cap
    double m_mOfD{0.0};                 // minimum-statistics bias interpolation M(D)
    double m_mOfV{0.0};                 // minimum-statistics bias interpolation M(V)
    double m_noiseSlopeMax[4]{};         // guarded upward noise-floor slopes
    int m_recentSpeechFrames{0};         // arms the post-speech noise release bridge
    int m_recentSpeechFramesMax{1};
    int m_releaseCandidateFrames{0};
    int m_releaseCandidateFramesMin{1};
    int m_releaseNoiseFrames{0};
    int m_releaseNoiseFramesMax{1};
    int m_releaseNpeMethod{-1};
    bool m_releaseNoiseRefreshed{false};
    double m_releaseNoiseDecay{0.0};
    double m_releaseBaselineAlpha{0.0};
    bool m_releaseBaselineInitialized{false};
    std::vector<double> m_releaseBaselinePsd;
    std::vector<double> m_releaseNoisePsd;
    // Speech-presence MMSE estimator (WDSP LambdaDs / NPE method 1)
    std::vector<double> m_mmseNoisePsd;
    std::vector<double> m_mmsePbar;
    double m_mmseAlphaPow{0.8};
    double m_mmseAlphaPbar{0.9};

    // Non-stationary minima estimator (WDSP LambdaDl / NPE method 2)
    std::vector<double> m_nstatPower;
    std::vector<double> m_nstatPowerMin;
    std::vector<double> m_nstatSpeechProbability;
    std::vector<double> m_nstatNoisePsd;
    double m_nstatEta{0.7};
    double m_nstatGamma{0.998};
    double m_nstatBeta{0.8};
    double m_nstatAlphaD{0.85};
    double m_nstatAlphaP{0.2};
    int m_nstatLowFrequencyBin{0};
    int m_nstatMidFrequencyBin{0};

    // Decision-directed gain memory, scaled to the actual hop duration.
    double m_gainAlpha{0.985};
    double m_gainDecreaseSmooth{0.5};

    // Gain state per-bin
    std::vector<double> m_prevMask;     // previous frame gain mask
    std::vector<double> m_prevGamma;    // previous frame a-posteriori SNR
    std::vector<double> m_mask;         // current gain mask
    std::vector<double> m_smoothMask;   // temporally smoothed gain (anti-musical-noise)
    std::vector<double> m_lambdaY;      // current frame signal PSD
    double m_currentWet{0.0};            // startup dry/wet blend for current frame

    // Startup ramp
    int m_frameCount{0};                // frames processed since reset
    int m_rampFrames{1};                // one second at the configured hop rate

    // ── Algorithm constants (fixed) ─────────────────────────────────────
    static constexpr double GammaMax   = 40.0;    // linear a-posteriori SNR cap
    static constexpr double XiMin      = 1e-4;    // a-priori SNR floor
    static constexpr double EpsFloor   = 1e-300;  // match WDSP eps_floor
    static constexpr double InvQeqMax  = 0.5;

    // ── User-adjustable parameters (atomic for audio thread safety) ───
    std::atomic<double> m_gainMax{1.0};     // cap gain — noise REDUCTION, never amplify above input (#1507)
    std::atomic<double> m_gainFloor{0.00};  // no forced residual; user can raise Naturalness if needed
    std::atomic<double> m_qSpp{0.2};        // speech presence probability prior
    std::atomic<double> m_gainSmooth{0.85}; // temporal gain smoothing (anti-musical-noise)
    std::atomic<int>    m_gainMethod{2};    // 0=Linear, 1=Log, 2=Gamma, 3=Trained
    std::atomic<int>    m_npeMethod{0};     // 0=OSMS, 1=MMSE, 2=NSTAT
    std::atomic<bool>   m_aeFilter{true};   // artifact elimination post-processing

    // AE filter state (per-bin)
    std::vector<double> m_aeMask;           // smoothed AE gain mask
    std::vector<double> m_aePrefix;         // prefix sums for adaptive frequency averaging

    // ── Internal methods ───────────────────────────────────────────────
    void initWindow();
    void processFrame();
    bool updateMaskFromCurrentFrame();
    void synthesizeCurrentFrequencyBinsWithMask();
    void synthesizeCurrentFrameWithMask();

    // Noise estimation (keeps every estimator warm, then selects one)
    void estimateNoise();
    void estimateNoiseOsms();   // method 0: Optimal Smoothing Minimum Statistics
    void estimateNoiseMmse();   // method 1: MMSE noise estimator
    void estimateNoiseNstat();  // method 2: Non-stationary noise estimator
    void updateSpeechReleaseState();
    void applySpeechReleaseEstimate();

    // Spectral gain computation (dispatches on m_gainMethod)
    void computeGain();
    void computeGainWiener();   // legacy Aether method 0 comparison
    void computeGainLinear();   // method 0: Gaussian, linear amplitude
    void computeGainLog();      // method 1: Gaussian, log amplitude
    void computeGainGamma();    // method 2: Gamma-prior GG x GGS lookup
    void computeGainTrained();  // method 3: Aether experimental approximation

    // Artifact elimination post-processing
    void applyAeFilter();

    // Exponentially-scaled modified Bessel functions of the first kind.
    // bessI0e(x) = exp(-|x|) * I0(x),  bessI1e(x) = exp(-|x|) * I1(x).
    // The large-x branch is 1/sqrt(|x|) * poly — no exp() computed, no overflow.
    static double bessI0e(double x);
    static double bessI1e(double x);
};

} // namespace AetherSDR
