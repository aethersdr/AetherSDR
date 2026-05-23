/*  SpectralNR.cpp

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

#include "SpectralNR.h"
#include "LogManager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <numeric>

namespace MasterSDR {

std::mutex SpectralNR::s_fftwMutex;

namespace {

std::string wisdomPathForDirectory(const std::string& directory)
{
    std::string wisdomFile = directory;
    if (!wisdomFile.empty() && wisdomFile.back() != '/' && wisdomFile.back() != '\\')
        wisdomFile += '/';
    wisdomFile += "MasterSDR_fftw_wisdom";
    return wisdomFile;
}

} // namespace

// ─── Construction / Reset ──────────────────────────────────────────────────────

SpectralNR::SpectralNR(int fftSize, int sampleRate)
    : m_fftSize(fftSize)
    , m_hopSize(fftSize / 2)
    , m_msize(fftSize / 2 + 1)
    , m_sampleRate(sampleRate)
{
    // OSMS sub-window geometry: D = U*V frames ~ 2 s at given hop rate
    const double framesPerSec = static_cast<double>(sampleRate) / m_hopSize;
    const double targetSec = 2.0;
    m_D = static_cast<int>(targetSec * framesPerSec);
    m_U = 8;
    m_V = std::max(1, m_D / m_U);
    m_D = m_U * m_V;

    // Allocate overlap-add accumulators
    m_inAccum.resize(fftSize * 4, 0.0);
    m_outAccum.resize(fftSize * 4, 0.0);

    m_window.resize(fftSize);
    m_fftIn.resize(fftSize);
    m_ifftOut.resize(fftSize);

    // Frequency-domain bins
    m_freqRe.resize(m_msize);
    m_freqIm.resize(m_msize);
    m_gainRe.resize(m_msize);
    m_gainIm.resize(m_msize);

#ifdef HAVE_FFTW3
    // FFTW-allocated complex arrays (16-byte aligned)
    m_fftOut = fftw_alloc_complex(m_msize);
    m_ifftIn = fftw_alloc_complex(m_msize);

    // Create plans — uses wisdom if available for optimal performance.
    // FFTW_MEASURE is used here: fast enough for size 256 even without
    // prior wisdom, and will use wisdom when it's been generated.
    // Lock: FFTW plan creation is NOT thread-safe (#467)
    {
        std::lock_guard<std::mutex> lock(s_fftwMutex);
        m_planFwd = fftw_plan_dft_r2c_1d(fftSize, m_fftIn.data(),
                                          m_fftOut, FFTW_MEASURE);
        m_planRev = fftw_plan_dft_c2r_1d(fftSize, m_ifftIn,
                                          m_ifftOut.data(), FFTW_MEASURE);
    }
    if (!m_planFwd || !m_planRev) {
        qCWarning(lcDsp) << "SpectralNR: FFTW plan creation failed — NR2 will not function";
        m_planFailed = true;
    }
#else
    // Fallback: built-in radix-2 FFT
    m_fftScratchRe.resize(fftSize);
    m_fftScratchIm.resize(fftSize);
    m_fftScratchRe2.resize(fftSize);
    m_fftScratchIm2.resize(fftSize);
    initBitReversal();
#endif

    // Noise estimation state
    m_noisePsd.resize(m_msize);
    m_smoothPsd.resize(m_msize);
    m_pMin.resize(m_msize);
    m_pBar.resize(m_msize);
    m_p2Bar.resize(m_msize);
    m_alphaOpt.resize(m_msize);
    m_alphaHat.resize(m_msize);
    m_actMin.resize(m_msize);
    m_actMinSub.resize(m_msize);
    m_lminFlag.resize(m_msize, 0);

    m_actMinBuf.resize(m_U);
    for (auto& v : m_actMinBuf)
        v.resize(m_msize, 1e30);

    // Gain state
    m_prevMask.resize(m_msize, 1.0);
    m_prevGamma.resize(m_msize, 1.0);
    m_mask.resize(m_msize, 1.0);
    m_smoothMask.resize(m_msize, 1.0);
    m_lambdaY.resize(m_msize);
    m_aeMask.resize(m_msize, 1.0);

    initWindow();
    reset();
}

SpectralNR::~SpectralNR()
{
#ifdef HAVE_FFTW3
    {
        std::lock_guard<std::mutex> lock(s_fftwMutex);
        if (m_planFwd) fftw_destroy_plan(m_planFwd);
        if (m_planRev) fftw_destroy_plan(m_planRev);
    }
    if (m_fftOut)  fftw_free(m_fftOut);
    if (m_ifftIn)  fftw_free(m_ifftIn);
#endif
}

void SpectralNR::reset()
{
    std::fill(m_inAccum.begin(), m_inAccum.end(), 0.0);
    std::fill(m_outAccum.begin(), m_outAccum.end(), 0.0);
    m_inWritePos = 0;
    m_inReadPos = 0;
    m_samplesAccum = 0;

    // Critical: output write position must lead read position by (fftSize - hopSize)
    // so that the overlap-add accumulates both frame contributions before samples
    // are read.  This matches WDSP's init_oainidx = fsize - incr.
    m_outWritePos = m_fftSize - m_hopSize;
    m_outReadPos = 0;

    // Start with a HIGH noise estimate — gains will be < 1 during convergence,
    // producing gentle suppression rather than amplification spikes.
    // The OSMS tracker will converge downward to the true noise floor in ~2s.
    constexpr double initNoise = 1.0;
    std::fill(m_noisePsd.begin(), m_noisePsd.end(), initNoise);
    std::fill(m_smoothPsd.begin(), m_smoothPsd.end(), initNoise);
    std::fill(m_pMin.begin(), m_pMin.end(), initNoise);
    std::fill(m_pBar.begin(), m_pBar.end(), initNoise);
    std::fill(m_p2Bar.begin(), m_p2Bar.end(), initNoise * initNoise);
    std::fill(m_alphaOpt.begin(), m_alphaOpt.end(), AlphaMax);
    std::fill(m_alphaHat.begin(), m_alphaHat.end(), AlphaMax);
    std::fill(m_actMin.begin(), m_actMin.end(), 1e30);
    std::fill(m_actMinSub.begin(), m_actMinSub.end(), 1e30);
    std::fill(m_lminFlag.begin(), m_lminFlag.end(), 0);

    for (auto& v : m_actMinBuf)
        std::fill(v.begin(), v.end(), 1e30);

    std::fill(m_prevMask.begin(), m_prevMask.end(), 1.0);
    std::fill(m_prevGamma.begin(), m_prevGamma.end(), 1.0);
    std::fill(m_mask.begin(), m_mask.end(), 1.0);
    std::fill(m_smoothMask.begin(), m_smoothMask.end(), 1.0);
    std::fill(m_aeMask.begin(), m_aeMask.end(), 1.0);

    m_alphaC = 1.0;
    m_subwc = 1;
    m_ambIdx = 0;
    m_frameCount = 0;
}

void SpectralNR::initWindow()
{
    // Hann window — exact COLA property with 50 % overlap.
    // Applied as sqrt(Hann) at both analysis and synthesis so that
    // w_a[i]*w_s[i] = Hann[i], and sum_k Hann[n-kH] = 1 for all n.
    const double N = static_cast<double>(m_fftSize);
    for (int i = 0; i < m_fftSize; ++i) {
        double hann = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * i / N));
        m_window[i] = std::sqrt(hann);
    }
}

// ─── FFTW Wisdom ───────────────────────────────────────────────────────────────

bool SpectralNR::loadWisdom(const std::string& directory)
{
#ifdef HAVE_FFTW3
    const std::string wisdomFile = wisdomPathForDirectory(directory);
    std::lock_guard<std::mutex> lock(s_fftwMutex);
    return fftw_import_wisdom_from_filename(wisdomFile.c_str()) != 0;
#else
    (void)directory;
    return false;
#endif
}

bool SpectralNR::generateWisdom(const std::string& directory,
                                WisdomProgressCb progress)
{
#ifdef HAVE_FFTW3
    const std::string wisdomFile = wisdomPathForDirectory(directory);

    if (loadWisdom(directory)) {
        return false;  // wisdom loaded from file — no generation needed
    }

    // Try to import Thetis/WDSP wisdom (compatible FFTW3 format)
    // This gives us a head start if Thetis is installed
#ifdef _WIN32
    {
        const char* appData = std::getenv("APPDATA");
        if (appData) {
            std::string thetisWisdom = std::string(appData)
                + "\\OpenHPSDR\\Thetis-x64\\wdspWisdom00";
            std::lock_guard<std::mutex> lock(s_fftwMutex);
            if (fftw_import_wisdom_from_filename(thetisWisdom.c_str())) {
                // Save as our own so we don't depend on Thetis in future
                fftw_export_wisdom_to_filename(wisdomFile.c_str());
                return false;
            }
        }
    }
#endif

    // ── Full wisdom generation (matches Thetis: sizes 64 through 262144) ───
    // This takes several minutes on first run.  FFTW_PATIENT produces
    // highly optimised plans for each size.
    constexpr int maxSize = 262144;
    auto* cbuf = fftw_alloc_complex(maxSize);
    auto* rbuf = static_cast<double*>(fftw_malloc(maxSize * sizeof(double)));

    // Count total steps for progress reporting
    // Sizes: 64, 128, 256, ... 262144 = 13 sizes × 4 plan types = 52 steps
    int totalSteps = 0;
    for (int s = 64; s <= maxSize; s *= 2) totalSteps += 4;
    int step = 0;

    for (int psize = 64; psize <= maxSize; psize *= 2) {
        // Lock per-plan to avoid holding the mutex for minutes while still
        // preventing concurrent plan creation (#467)

        // 1. Complex forward
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX FORWARD FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_1d(psize, cbuf, cbuf,
                                            FFTW_FORWARD, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 2. Complex backward (same size)
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX BACKWARD FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_1d(psize, cbuf, cbuf,
                                            FFTW_BACKWARD, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 3. Real-to-complex forward
        if (progress) progress(step, totalSteps,
            "Computing REAL-TO-COMPLEX FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_r2c_1d(psize, rbuf, cbuf, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 4. Complex-to-real inverse
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX-TO-REAL FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_c2r_1d(psize, cbuf, rbuf, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");
    }

    {
        std::lock_guard<std::mutex> lock(s_fftwMutex);
        if (!fftw_export_wisdom_to_filename(wisdomFile.c_str()))
            qCWarning(lcDsp) << "SpectralNR: failed to export FFTW wisdom to"
                             << QString::fromStdString(wisdomFile);
    }
    fftw_free(rbuf);
    fftw_free(cbuf);
    return true;  // wisdom was generated
#else
    (void)directory;
    (void)progress;
    return false;
#endif
}

// ─── Main Processing ───────────────────────────────────────────────────────────

void SpectralNR::process(const float* input, float* output, int numSamples)
{
    const int accSize = static_cast<int>(m_inAccum.size());
    const int outSize = static_cast<int>(m_outAccum.size());

    // Push input samples into accumulator (float32 -> float64)
    for (int i = 0; i < numSamples; ++i) {
        m_inAccum[m_inWritePos] = static_cast<double>(input[i]);
        m_inWritePos = (m_inWritePos + 1) % accSize;
    }
    m_samplesAccum += numSamples;

    // Process complete FFT frames (50% overlap -> advance by hopSize each frame)
    while (m_samplesAccum >= m_fftSize) {
        // Extract frame and apply analysis window
        for (int i = 0; i < m_fftSize; ++i) {
            int idx = (m_inReadPos + i) % accSize;
            m_fftIn[i] = m_window[i] * m_inAccum[idx];
        }
        m_inReadPos = (m_inReadPos + m_hopSize) % accSize;
        m_samplesAccum -= m_hopSize;

        processFrame();

        // Overlap-add: apply synthesis window and accumulate
        for (int i = 0; i < m_fftSize; ++i) {
            int idx = (m_outWritePos + i) % outSize;
            m_outAccum[idx] += m_window[i] * m_ifftOut[i];
        }
        m_outWritePos = (m_outWritePos + m_hopSize) % outSize;
    }

    // Read output samples (float64 -> float32), clearing consumed positions
    for (int i = 0; i < numSamples; ++i) {
        output[i] = static_cast<float>(m_outAccum[m_outReadPos]);
        m_outAccum[m_outReadPos] = 0.0;
        m_outReadPos = (m_outReadPos + 1) % outSize;
    }
}

void SpectralNR::processFrame()
{
#ifdef HAVE_FFTW3
    if (m_planFailed) return;  // FFTW plans failed — pass audio through unmodified

    // Forward FFT via FFTW (real-to-complex, in-place from m_fftIn)
    fftw_execute(m_planFwd);

    // Unpack FFTW complex output into separate re/im arrays
    for (int k = 0; k < m_msize; ++k) {
        m_freqRe[k] = m_fftOut[k][0];
        m_freqIm[k] = m_fftOut[k][1];
    }
#else
    fftForward(m_fftIn.data(), m_freqRe.data(), m_freqIm.data());
#endif

    // Compute signal power spectrum |Y(k)|^2
    for (int k = 0; k < m_msize; ++k)
        m_lambdaY[k] = m_freqRe[k] * m_freqRe[k] + m_freqIm[k] * m_freqIm[k];

    // Noise estimation (OSMS)
    estimateNoise();

    // Compute spectral gain mask
    computeGain();

    // Artifact elimination post-processing (smooths gain mask to reduce musical noise)
    if (m_aeFilter.load())
        applyAeFilter();

    // Temporal gain smoothing — prevents rapid per-bin fluctuations
    // that cause "musical noise" clicks and glitches.
    for (int k = 0; k < m_msize; ++k)
    {
        const double gs = m_gainSmooth.load();
        m_smoothMask[k] = gs * m_smoothMask[k] + (1.0 - gs) * m_mask[k];
    }

    // Startup ramp: crossfade from dry (gain=1) to processed over ~1 second
    // to avoid transients while the noise estimator converges.
    ++m_frameCount;
    double wet = (m_frameCount >= RampFrames)
        ? 1.0
        : static_cast<double>(m_frameCount) / RampFrames;

    // Apply smoothed gain to frequency bins (with dry/wet blend during startup)
    for (int k = 0; k < m_msize; ++k) {
        double g = wet * m_smoothMask[k] + (1.0 - wet) * 1.0;
        m_gainRe[k] = g * m_freqRe[k];
        m_gainIm[k] = g * m_freqIm[k];
    }

#ifdef HAVE_FFTW3
    // Pack into FFTW complex input for inverse FFT
    for (int k = 0; k < m_msize; ++k) {
        m_ifftIn[k][0] = m_gainRe[k];
        m_ifftIn[k][1] = m_gainIm[k];
    }

    // Inverse FFT via FFTW (complex-to-real)
    fftw_execute(m_planRev);

    // FFTW c2r does NOT divide by N — we must scale
    const double invN = 1.0 / m_fftSize;
    for (int i = 0; i < m_fftSize; ++i)
        m_ifftOut[i] *= invN;
#else
    fftInverse(m_gainRe.data(), m_gainIm.data(), m_ifftOut.data());
#endif
}

// ─── Noise Estimation (dispatches on m_npeMethod) ─────────────────────────────

void SpectralNR::estimateNoise()
{
    switch (m_npeMethod.load()) {
    case 1:  estimateNoiseMmse();  return;
    case 2:  estimateNoiseNstat(); return;
    default: estimateNoiseOsms();  return;
    }
}

// ─── OSMS Noise Estimation (from WDSP LambdaD) ────────────────────────────────

void SpectralNR::estimateNoiseOsms()
{
    // Smoothing time constant (matches WDSP)
    const double tau = -128.0 / 8000.0 / std::log(0.7);
    const double alphaCSmooth = std::exp(static_cast<double>(-m_hopSize) /
                                         (m_sampleRate * tau));

    // ── Pass 1: Global sums for SNR estimates ─────────────────────────
    double sumPrevP = 0.0, sumLambdaY = 0.0, sumSigma2N = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        sumPrevP   += m_smoothPsd[k];
        sumLambdaY += m_lambdaY[k];
        sumSigma2N += m_noisePsd[k];
    }
    if (sumSigma2N < EpsFloor) sumSigma2N = EpsFloor;
    if (sumLambdaY < EpsFloor) sumLambdaY = EpsFloor;

    // ── Pass 2: Per-bin optimal smoothing + smoothed periodogram ──────
    for (int k = 0; k < m_msize; ++k) {
        double sigma = std::max(m_noisePsd[k], EpsFloor);
        double f0 = m_smoothPsd[k] / sigma - 1.0;
        m_alphaOpt[k] = 1.0 / (1.0 + f0 * f0);
    }

    double snr = sumPrevP / sumSigma2N;
    double alphaMin = std::min(0.3, std::pow(snr, SnrqExp));

    for (int k = 0; k < m_msize; ++k)
        if (m_alphaOpt[k] < alphaMin)
            m_alphaOpt[k] = alphaMin;

    double f1 = sumPrevP / sumLambdaY - 1.0;
    double alphaCtilda = 1.0 / (1.0 + f1 * f1);
    m_alphaC = alphaCSmooth * m_alphaC +
               (1.0 - alphaCSmooth) * std::max(alphaCtilda, AlphaCMin);

    double f2 = AlphaMax * m_alphaC;
    for (int k = 0; k < m_msize; ++k) {
        m_alphaHat[k] = f2 * m_alphaOpt[k];
        m_smoothPsd[k] = m_alphaHat[k] * m_smoothPsd[k] +
                         (1.0 - m_alphaHat[k]) * m_lambdaY[k];
    }

    // ── Pass 3: Variance estimation + invQbar accumulation ────────────
    double invQbar = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        double beta = std::min(BetaMax, m_alphaHat[k] * m_alphaHat[k]);
        m_pBar[k]  = beta * m_pBar[k]  + (1.0 - beta) * m_smoothPsd[k];
        m_p2Bar[k] = beta * m_p2Bar[k] + (1.0 - beta) * m_smoothPsd[k] * m_smoothPsd[k];

        double varHat = m_p2Bar[k] - m_pBar[k] * m_pBar[k];
        double sigma2 = std::max(m_noisePsd[k], EpsFloor);
        double invQeq = std::min(InvQeqMax, varHat / (2.0 * sigma2 * sigma2));
        invQbar += invQeq;
    }
    invQbar /= static_cast<double>(m_msize);

    // ── Pass 4: Bias correction + minimum tracking (uses final invQbar)
    double bc = 1.0 + 2.46 * std::sqrt(invQbar);

    for (int k = 0; k < m_msize; ++k) {
        double sigma2 = std::max(m_noisePsd[k], EpsFloor);
        double varHat = m_p2Bar[k] - m_pBar[k] * m_pBar[k];
        double invQeq = std::min(InvQeqMax, varHat / (2.0 * sigma2 * sigma2));
        double Qeq = 1.0 / std::max(invQeq, 1e-10);

        double MofD = static_cast<double>(m_D);
        double MofV = static_cast<double>(m_V);
        double QeqTilda = (Qeq - 2.0 * MofD) / std::max(1.0 - MofD, 1e-10);
        double bmin = 1.0 + 2.0 * (m_D - 1) / std::max(QeqTilda, 1e-10);

        double QeqTildaSub = (Qeq - 2.0 * MofV) / std::max(1.0 - MofV, 1e-10);
        double bminSub = 1.0 + 2.0 * (m_V - 1) / std::max(QeqTildaSub, 1e-10);

        double f3 = m_smoothPsd[k] * bmin * bc;
        if (f3 < m_actMin[k]) {
            m_actMin[k] = f3;
            m_actMinSub[k] = m_smoothPsd[k] * bminSub * bc;
            m_lminFlag[k] = 0;
        }
    }

    // ── Sub-window rotation ───────────────────────────────────────────
    if (m_subwc >= m_V) {
        for (int k = 0; k < m_msize; ++k) {
            m_actMinBuf[m_ambIdx][k] = m_actMin[k];
            double minVal = 1e30;
            for (int u = 0; u < m_U; ++u)
                minVal = std::min(minVal, m_actMinBuf[u][k]);
            m_pMin[k] = minVal;

            if (m_lminFlag[k] && m_actMinSub[k] < 8.0 * m_pMin[k] &&
                m_actMinSub[k] > m_pMin[k]) {
                m_pMin[k] = m_actMinSub[k];
                for (int u = 0; u < m_U; ++u)
                    m_actMinBuf[u][k] = m_actMinSub[k];
            }
            m_lminFlag[k] = 0;
            m_actMin[k] = 1e30;
            m_actMinSub[k] = 1e30;
        }
        m_ambIdx = (m_ambIdx + 1) % m_U;
        m_subwc = 1;
    } else {
        if (m_subwc > 1) {
            for (int k = 0; k < m_msize; ++k) {
                if (m_actMinSub[k] < m_pMin[k]) {
                    m_lminFlag[k] = 1;
                    m_noisePsd[k] = std::min(m_actMinSub[k], m_pMin[k]);
                    m_pMin[k] = m_noisePsd[k];
                }
            }
        }
        ++m_subwc;
    }

    // Final noise estimate = minimum statistics
    for (int k = 0; k < m_msize; ++k)
        m_noisePsd[k] = m_pMin[k];
}

// ─── Spectral Gain Computation (dispatches on m_gainMethod) ───────────────────

void SpectralNR::computeGain()
{
    switch (m_gainMethod.load()) {
    case 0:  computeGainLinear();  return;
    case 1:  computeGainLog();     return;
    case 3:  computeGainTrained(); return;
    default: computeGainGamma();   return;
    }
}

// ─── Ephraim-Malah MMSE-LSA Gain (Gamma method — default) ────────────────────

void SpectralNR::computeGainGamma()
{
    constexpr double gf1p5 = 0.8862269254527580;  // sqrt(pi) / 2

    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        // A posteriori SNR
        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        // A priori SNR (decision-directed)
        double epsHat = Alpha * m_prevMask[k] * m_prevMask[k] * m_prevGamma[k]
                      + (1.0 - Alpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Ephraim-Malah MMSE-LSA
        double ehr = epsHat / (1.0 + epsHat);
        double v = ehr * gamma;

        double gain = gf1p5 * std::sqrt(v) / std::max(gamma, EpsFloor)
                    * std::exp(-0.5 * v)
                    * ((1.0 + v) * bessI0(0.5 * v) + v * bessI1(0.5 * v));

        // Speech presence probability weighting
        {
            double v2 = std::min(v, 700.0);
            double eta = gain * gain * m_lambdaY[k] / lambdaD;
            const double qspp = m_qSpp.load();
            double eps = eta / (1.0 - qspp);
            double witchHat = (1.0 - qspp) / qspp * std::exp(v2) / (1.0 + eps);
            gain *= witchHat / (1.0 + witchHat);
        }

        // Clamp and NaN guard
        const double gmax = m_gainMax.load();
        if (gain > gmax) gain = gmax;
        if (gain != gain) gain = 0.01;  // NaN

        m_mask[k] = gain;
        m_prevGamma[k] = gamma;
        m_prevMask[k] = gain;
    }
}

// ─── Linear Gain Method ───────────────────────────────────────────────────────
// Simple Wiener filter: G = xi / (1 + xi), operating on linear amplitude.
// Reference: WDSP emnr.c gain_method == 0

void SpectralNR::computeGainLinear()
{
    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        // A posteriori SNR
        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        // A priori SNR (decision-directed)
        double epsHat = Alpha * m_prevMask[k] * m_prevMask[k] * m_prevGamma[k]
                      + (1.0 - Alpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Wiener gain: xi / (1 + xi)
        double gain = epsHat / (1.0 + epsHat);

        // Clamp and NaN guard
        const double gmax = m_gainMax.load();
        if (gain > gmax) gain = gmax;
        if (gain != gain) gain = 0.01;

        m_mask[k] = gain;
        m_prevGamma[k] = gamma;
        m_prevMask[k] = gain;
    }
}

// ─── Log-Spectral Amplitude Gain Method ───────────────────────────────────────
// Ephraim-Malah log-spectral amplitude estimator.
// Reference: WDSP emnr.c gain_method == 1

void SpectralNR::computeGainLog()
{
    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        double epsHat = Alpha * m_prevMask[k] * m_prevMask[k] * m_prevGamma[k]
                      + (1.0 - Alpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Log-spectral amplitude: G = xi/(1+xi) * exp(0.5 * E1(v))
        // where v = xi*gamma/(1+xi) and E1 is the exponential integral.
        // Approximate E1(v) ≈ -log(v) - 0.5772 for small v,
        // E1(v) ≈ exp(-v)/v for large v.
        double v = epsHat * gamma / (1.0 + epsHat);
        double expInt;
        if (v < 0.001)
            expInt = -std::log(std::max(v, EpsFloor)) - 0.5772156649;
        else if (v > 20.0)
            expInt = std::exp(-v) / v;
        else {
            // Series/continued fraction for intermediate values
            // Use the relation E1(v) = -Ei(-v) with rational approximation
            double sum = 0.0;
            double term = 1.0;
            for (int n = 1; n <= 50; ++n) {
                term *= -v / n;
                sum += term / n;
            }
            expInt = -0.5772156649 - std::log(std::max(v, EpsFloor)) - sum;
        }

        double gain = (epsHat / (1.0 + epsHat)) * std::exp(0.5 * expInt);

        const double gmax = m_gainMax.load();
        if (gain > gmax) gain = gmax;
        if (gain != gain) gain = 0.01;

        m_mask[k] = gain;
        m_prevGamma[k] = gamma;
        m_prevMask[k] = gain;
    }
}

// ─── Trained Gain Method ──────────────────────────────────────────────────────
// Uses a piecewise-linear lookup derived from speech/noise statistics.
// The table approximates trained noise suppression curves from WDSP.
// Reference: WDSP emnr.c gain_method == 3

void SpectralNR::computeGainTrained()
{
    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        double epsHat = Alpha * m_prevMask[k] * m_prevMask[k] * m_prevGamma[k]
                      + (1.0 - Alpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Trained suppression curve: piecewise function of a-priori SNR (dB)
        double xiDb = 10.0 * std::log10(std::max(epsHat, EpsFloor));

        double gain;
        if (xiDb < -20.0)
            gain = 0.01;          // heavy suppression in deep noise
        else if (xiDb < -10.0)
            gain = 0.01 + 0.049 * (xiDb + 20.0) / 10.0;  // 0.01 → 0.06
        else if (xiDb < 0.0)
            gain = 0.06 + 0.34 * (xiDb + 10.0) / 10.0;   // 0.06 → 0.40
        else if (xiDb < 10.0)
            gain = 0.40 + 0.50 * xiDb / 10.0;             // 0.40 → 0.90
        else
            gain = 0.90 + 0.10 * std::min((xiDb - 10.0) / 10.0, 1.0); // → 1.0

        const double gmax = m_gainMax.load();
        if (gain > gmax) gain = gmax;
        if (gain != gain) gain = 0.01;

        m_mask[k] = gain;
        m_prevGamma[k] = gamma;
        m_prevMask[k] = gain;
    }
}

// ─── MMSE Noise Estimator (NPE method 1) ─────────────────────────────────────
// Simplified MMSE noise power estimation: uses decision-directed smoothing
// of the periodogram weighted by speech absence probability.
// Reference: WDSP emnr.c npest == 1

void SpectralNR::estimateNoiseMmse()
{
    for (int k = 0; k < m_msize; ++k) {
        double sigma = std::max(m_noisePsd[k], EpsFloor);

        // A posteriori SNR
        double gamma = m_lambdaY[k] / sigma;

        // Speech absence probability (simple threshold-based)
        double pSa;
        if (gamma < 1.5)
            pSa = 0.95;   // likely noise-only
        else if (gamma < 3.0)
            pSa = 0.5;
        else
            pSa = 0.05;   // likely speech present

        // MMSE update: weight between current observation and previous estimate
        // When speech is absent (pSa high), trust the observation more
        double alpha = 0.98 * (1.0 - pSa) + 0.7 * pSa;
        m_noisePsd[k] = alpha * m_noisePsd[k] + (1.0 - alpha) * m_lambdaY[k];
    }
}

// ─── Non-Stationary Noise Estimator (NPE method 2) ───────────────────────────
// Tracks noise in non-stationary environments using a two-pass approach:
// fast adaptation when SNR is low, slow tracking otherwise.
// Reference: WDSP emnr.c npest == 2

void SpectralNR::estimateNoiseNstat()
{
    // Global SNR estimate for adaptation rate
    double sumY = 0.0, sumN = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        sumY += m_lambdaY[k];
        sumN += m_noisePsd[k];
    }
    if (sumN < EpsFloor) sumN = EpsFloor;
    double globalSnr = sumY / sumN;

    // Adaptation rate: faster when global SNR is low (noise-dominated)
    double alphaFast = 0.7;
    double alphaSlow = 0.995;
    double blend = std::min(std::max((globalSnr - 1.0) / 3.0, 0.0), 1.0);
    double alpha = alphaFast + blend * (alphaSlow - alphaFast);

    for (int k = 0; k < m_msize; ++k) {
        double localGamma = m_lambdaY[k] / std::max(m_noisePsd[k], EpsFloor);

        // Per-bin adaptation: if local SNR is low, use faster update
        double alphaK = alpha;
        if (localGamma < 1.5)
            alphaK = std::min(alphaK, 0.85);

        m_noisePsd[k] = alphaK * m_noisePsd[k] + (1.0 - alphaK) * m_lambdaY[k];
    }
}

// ─── Artifact Elimination Filter ──────────────────────────────────────────────
// Smooths the gain mask across frequency bins to reduce musical noise
// artifacts (isolated spectral peaks in the gain). Uses a 3-bin weighted
// average followed by a minimum constraint with neighboring bins.
// Reference: WDSP emnr.c ae_run code path

void SpectralNR::applyAeFilter()
{
    // Pass 1: 3-bin weighted smoothing of the gain mask
    // Store smoothed result in m_aeMask to avoid modifying m_mask during iteration
    m_aeMask[0] = 0.75 * m_mask[0] + 0.25 * m_mask[1];
    for (int k = 1; k < m_msize - 1; ++k)
        m_aeMask[k] = 0.25 * m_mask[k - 1] + 0.50 * m_mask[k] + 0.25 * m_mask[k + 1];
    m_aeMask[m_msize - 1] = 0.25 * m_mask[m_msize - 2] + 0.75 * m_mask[m_msize - 1];

    // Pass 2: constrain each bin's gain to be no more than 1.5× its
    // smoothed neighbor average — eliminates isolated spectral peaks
    for (int k = 0; k < m_msize; ++k) {
        double limit = 1.5 * m_aeMask[k];
        if (m_mask[k] > limit)
            m_mask[k] = limit;
    }
}

// ─── Modified Bessel Functions ────────────────────────────────────────────────
// Polynomial approximations of I0(x) and I1(x) from Abramowitz & Stegun,
// "Handbook of Mathematical Functions" (1964), formulas 9.8.1 and 9.8.2.
// A&S is a U.S. government work and in the public domain.

double SpectralNR::bessI0(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
    }
    double t = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double SpectralNR::bessI1(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    double t = 3.75 / ax;
    double val = (std::exp(ax) / std::sqrt(ax))
               * (0.39894228 + t * (-0.03988024 + t * (-0.00362018
                + t * (0.00163801 + t * (-0.01031555 + t * (0.02282967
                + t * (-0.02895312 + t * (0.01787654 - t * 0.00420059))))))));
    return x < 0.0 ? -val : val;
}

// ─── Fallback Radix-2 FFT (when FFTW3 is not available) ───────────────────────

#ifndef HAVE_FFTW3

void SpectralNR::initBitReversal()
{
    int n = m_fftSize;
    m_bitRev.resize(n);
    int bits = 0;
    for (int tmp = n; tmp > 1; tmp >>= 1) ++bits;
    for (int i = 0; i < n; ++i) {
        int rev = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                rev |= 1 << (bits - 1 - b);
        m_bitRev[i] = rev;
    }
}

void SpectralNR::fftForward(const double* timeIn, double* re, double* im)
{
    int n = m_fftSize;

    std::fill(m_fftScratchIm.begin(), m_fftScratchIm.end(), 0.0);
    for (int i = 0; i < n; ++i)
        m_fftScratchRe[m_bitRev[i]] = timeIn[i];

    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2;
        double angle = -2.0 * std::numbers::pi / len;
        double wRe = std::cos(angle);
        double wIm = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < half; ++j) {
                int a = i + j;
                int b = a + half;
                double tRe = curRe * m_fftScratchRe[b] - curIm * m_fftScratchIm[b];
                double tIm = curRe * m_fftScratchIm[b] + curIm * m_fftScratchRe[b];
                m_fftScratchRe[b] = m_fftScratchRe[a] - tRe;
                m_fftScratchIm[b] = m_fftScratchIm[a] - tIm;
                m_fftScratchRe[a] += tRe;
                m_fftScratchIm[a] += tIm;
                double newRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newRe;
            }
        }
    }

    for (int k = 0; k < m_msize; ++k) {
        re[k] = m_fftScratchRe[k];
        im[k] = m_fftScratchIm[k];
    }
}

void SpectralNR::fftInverse(const double* re, const double* im, double* timeOut)
{
    int n = m_fftSize;

    for (int k = 0; k < m_msize; ++k) {
        m_fftScratchRe[k] = re[k];
        m_fftScratchIm[k] = im[k];
    }
    for (int k = 1; k < n / 2; ++k) {
        m_fftScratchRe[n - k] =  re[k];
        m_fftScratchIm[n - k] = -im[k];
    }

    for (int i = 0; i < n; ++i) {
        m_fftScratchRe2[m_bitRev[i]] = m_fftScratchRe[i];
        m_fftScratchIm2[m_bitRev[i]] = m_fftScratchIm[i];
    }

    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2;
        double angle = 2.0 * std::numbers::pi / len;
        double wRe = std::cos(angle);
        double wIm = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < half; ++j) {
                int a = i + j;
                int b = a + half;
                double tRe = curRe * m_fftScratchRe2[b] - curIm * m_fftScratchIm2[b];
                double tIm = curRe * m_fftScratchIm2[b] + curIm * m_fftScratchRe2[b];
                m_fftScratchRe2[b] = m_fftScratchRe2[a] - tRe;
                m_fftScratchIm2[b] = m_fftScratchIm2[a] - tIm;
                m_fftScratchRe2[a] += tRe;
                m_fftScratchIm2[a] += tIm;
                double newRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newRe;
            }
        }
    }

    double invN = 1.0 / n;
    for (int i = 0; i < n; ++i)
        timeOut[i] = m_fftScratchRe2[i] * invN;
}

#endif // !HAVE_FFTW3

} // namespace MasterSDR
