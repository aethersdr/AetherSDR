/*  SpectralNR.h

This file is part of MasterSDR.

Portions of this file are derived from WDSP (emnr.c):
  Copyright (C) 2015, 2025 Warren Pratt, NR0V
  https://github.com/TAPR/OpenHPSDR-wdsp

The WDSP-derived portions are licensed under the GNU General Public License
as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

MasterSDR integration and C++20/Qt6 adaptation:
  Copyright (C) 2024-2026 MasterSDR Contributors

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

namespace MasterSDR {

// Client-side spectral noise reduction using the Ephraim-Malah MMSE
// Log-Spectral Amplitude estimator with OSMS noise floor tracking.
// Derived from WDSP NR2 (emnr.c) by Warren Pratt, NR0V.
//
// Uses FFTW3 for FFT computation (with wisdom file for optimised plans)
// when available; falls back to a built-in radix-2 FFT otherwise.
//
// Processes mono int16 audio at 24 kHz.  The caller is responsible for
// stereo<->mono conversion (average L+R on input, duplicate on output).

class SpectralNR {
public:
    explicit SpectralNR(int fftSize = 256, int sampleRate = 24000);
    ~SpectralNR();

    SpectralNR(const SpectralNR&) = delete;
    SpectralNR& operator=(const SpectralNR&) = delete;

    // Feed mono float32 samples in, get noise-reduced mono float32 out.
    // Output buffer must be at least numSamples long.
    void process(const float* input, float* output, int numSamples);

    // Reset all internal state (call when toggling on or stream restarts).
    void reset();

    // User-adjustable parameters (thread-safe, called from main thread)
    void setGainMax(float v)    { m_gainMax.store(v); }
    void setQspp(float v)      { m_qSpp.store(v); }
    void setGainSmooth(float v) { m_gainSmooth.store(v); }
    float gainMax() const       { return m_gainMax.load(); }
    float qspp() const         { return m_qSpp.load(); }
    float gainSmooth() const    { return m_gainSmooth.load(); }

    // Gain method: 0=Linear, 1=Log, 2=Gamma (default, MMSE-LSA), 3=Trained
    void setGainMethod(int m)   { m_gainMethod.store(m); }
    int  gainMethod() const     { return m_gainMethod.load(); }

    // NPE method: 0=OSMS (default), 1=MMSE, 2=NSTAT
    void setNpeMethod(int m)    { m_npeMethod.store(m); }
    int  npeMethod() const      { return m_npeMethod.load(); }

    // AE filter: artifact elimination post-processing
    void setAeFilter(bool on)   { m_aeFilter.store(on); }
    bool aeFilter() const       { return m_aeFilter.load(); }

    int fftSize() const { return m_fftSize; }
#ifdef HAVE_FFTW3
    bool hasPlanFailed() const { return m_planFailed; }
#else
    bool hasPlanFailed() const { return false; }
#endif

    // Generate FFTW wisdom file for optimal FFT performance.
    // Call once on first use; subsequent runs load existing wisdom.
    // Returns true if wisdom was generated (slow — several minutes), false if loaded.
    // The progress callback receives (currentStep, totalSteps, description).
    using WisdomProgressCb = std::function<void(int, int, const std::string&)>;
    static bool loadWisdom(const std::string& directory);
    static bool generateWisdom(const std::string& directory,
                               WisdomProgressCb progress = nullptr);

private:
    // FFTW plan creation/destruction is NOT thread-safe. This mutex guards
    // all fftw_plan_*, fftw_destroy_plan, and wisdom import/export calls.
    // fftw_execute() is thread-safe and does not need the lock. (#467)
    static std::mutex s_fftwMutex;
    // FFT parameters
    int m_fftSize;
    int m_hopSize;          // fftSize / 2  (50 % overlap)
    int m_msize;            // fftSize / 2 + 1  (real-FFT bin count)
    int m_sampleRate;

    // Overlap-add accumulators
    std::vector<double> m_inAccum;      // circular input buffer
    int m_inWritePos{0};
    int m_inReadPos{0};
    int m_samplesAccum{0};

    std::vector<double> m_outAccum;     // overlap-add output ring
    int m_outWritePos{0};
    int m_outReadPos{0};

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

    // Noise estimation (OSMS) per-bin state
    std::vector<double> m_noisePsd;     // lambda_d  -- estimated noise PSD
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
    std::vector<int> m_lminFlag;
    int m_subwc{1};                     // sub-window counter
    int m_ambIdx{0};                    // circular index into actMinBuf
    int m_U{8};                         // number of sub-windows
    int m_V{15};                        // frames per sub-window
    int m_D;                            // U * V

    // Gain state per-bin
    std::vector<double> m_prevMask;     // previous frame gain mask
    std::vector<double> m_prevGamma;    // previous frame a-posteriori SNR
    std::vector<double> m_mask;         // current gain mask
    std::vector<double> m_smoothMask;   // temporally smoothed gain (anti-musical-noise)
    std::vector<double> m_lambdaY;      // current frame signal PSD

    // Startup ramp
    int m_frameCount{0};                // frames processed since reset
    static constexpr int RampFrames = 187; // ~1 second at 24kHz/128hop

    // ── Algorithm constants (fixed) ─────────────────────────────────────
    static constexpr double Alpha      = 0.98;    // decision-directed smoothing
    static constexpr double GammaMax   = 1e4;     // 40 dB cap on a-posteriori SNR
    static constexpr double XiMin      = 1e-4;    // a-priori SNR floor
    static constexpr double EpsFloor   = 1e-300;  // match WDSP eps_floor
    static constexpr double AlphaMax   = 0.96;    // OSMS max smoothing
    static constexpr double AlphaCMin  = 0.7;
    static constexpr double BetaMax    = 0.8;
    static constexpr double InvQeqMax  = 0.5;
    static constexpr double SnrqExp    = -0.25;

    // ── User-adjustable parameters (atomic for audio thread safety) ───
    std::atomic<double> m_gainMax{1.0};     // cap gain — noise REDUCTION, never amplify above input (#1507)
    std::atomic<double> m_qSpp{0.2};        // speech presence probability prior
    std::atomic<double> m_gainSmooth{0.85}; // temporal gain smoothing (anti-musical-noise)
    std::atomic<int>    m_gainMethod{2};    // 0=Linear, 1=Log, 2=Gamma, 3=Trained
    std::atomic<int>    m_npeMethod{0};     // 0=OSMS, 1=MMSE, 2=NSTAT
    std::atomic<bool>   m_aeFilter{true};   // artifact elimination post-processing

    // AE filter state (per-bin)
    std::vector<double> m_aeMask;           // smoothed AE gain mask

    // ── Internal methods ───────────────────────────────────────────────
    void initWindow();
    void processFrame();

    // Noise estimation (dispatches on m_npeMethod)
    void estimateNoise();
    void estimateNoiseOsms();   // method 0: Optimal Smoothing Minimum Statistics
    void estimateNoiseMmse();   // method 1: MMSE noise estimator
    void estimateNoiseNstat();  // method 2: Non-stationary noise estimator

    // Spectral gain computation (dispatches on m_gainMethod)
    void computeGain();
    void computeGainLinear();   // method 0
    void computeGainLog();      // method 1
    void computeGainGamma();    // method 2 (Ephraim-Malah MMSE-LSA)
    void computeGainTrained();  // method 3

    // Artifact elimination post-processing
    void applyAeFilter();

    // Modified Bessel functions of the first kind
    static double bessI0(double x);
    static double bessI1(double x);
};

} // namespace MasterSDR
