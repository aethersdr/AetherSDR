/*  SpectralNR.cpp

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

#include "SpectralNR.h"
#include "LogManager.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <numbers>
#include <numeric>

namespace AetherSDR {

std::mutex SpectralNR::s_fftwMutex;

namespace {

// Gamma-prior MMSE gain and speech-presence surfaces. The grid contains
// 241x241 samples from -30 dB through +30 dB in 0.25 dB increments. The
// offline generator in tools/generate_nr2_gamma_tables.cpp implements the
// published Fodor-Fingscheidt equations; this lossless payload preserves the
// public WDSP reference values used for behavioral compatibility.
constexpr int kGammaTableGridSize = 241;
constexpr int kGammaTablePlaneSize =
    kGammaTableGridSize * kGammaTableGridSize;
constexpr int kGammaTableValueCount = 2 * kGammaTablePlaneSize;
constexpr double kGammaTableSpeechAbsencePrior = 0.20;

#include "Nr2GammaTables.inc"

struct GammaGainTables {
    std::vector<double> values;
    bool valid{false};
};

const GammaGainTables& gammaGainTables()
{
    static const GammaGainTables tables = [] {
        GammaGainTables result;
        result.values.resize(kGammaTableValueCount);

        QByteArray encoded;
        encoded.reserve(kNr2GammaTablesEncodedSize);
        for (const char* chunk :
             kNr2GammaTablesCompressedBase64Chunks) {
            encoded.append(chunk);
        }
        const QByteArray shuffled = qUncompress(
            QByteArray::fromBase64(encoded));
        const qsizetype expectedBytes =
            static_cast<qsizetype>(kGammaTableValueCount)
            * static_cast<qsizetype>(sizeof(std::uint64_t));
        if (shuffled.size() != expectedBytes) {
            return result;
        }

        const auto* packed = reinterpret_cast<const unsigned char*>(
            shuffled.constData());
        std::vector<std::uint64_t> bitPatterns(kGammaTableValueCount);
        QByteArray rawBytes;
        rawBytes.resize(expectedBytes);
        for (int index = 0; index < kGammaTableValueCount; ++index) {
            std::uint64_t residual = 0;
            for (int byte = 0; byte < 8; ++byte) {
                residual |= static_cast<std::uint64_t>(
                    packed[byte * kGammaTableValueCount + index])
                    << (8 * byte);
            }

            const int planeIndex = index % kGammaTablePlaneSize;
            const int row = planeIndex / kGammaTableGridSize;
            const int column = planeIndex % kGammaTableGridSize;
            const std::uint64_t left = column > 0
                ? bitPatterns[index - 1] : 0;
            const std::uint64_t up = row > 0
                ? bitPatterns[index - kGammaTableGridSize] : 0;
            const std::uint64_t upperLeft = row > 0 && column > 0
                ? bitPatterns[index - kGammaTableGridSize - 1] : 0;
            const std::uint64_t bits = residual + left + up - upperLeft;
            bitPatterns[index] = bits;

            const double value = std::bit_cast<double>(bits);
            if (!std::isfinite(value) || value < 0.0) {
                return result;
            }
            result.values[index] = value;
            for (int byte = 0; byte < 8; ++byte) {
                rawBytes[8 * index + byte] = static_cast<char>(
                    bits >> (8 * byte));
            }
        }

        const QByteArray actualSha256 = QCryptographicHash::hash(
            rawBytes, QCryptographicHash::Sha256).toHex();
        const QByteArray expectedSha256(
            kNr2GammaTablesRawSha256,
            static_cast<qsizetype>(sizeof(kNr2GammaTablesRawSha256) - 1));
        result.valid = actualSha256 == expectedSha256;
        return result;
    }();
    return tables;
}

double gammaTableLookup(const GammaGainTables& tables,
                        int planeOffset,
                        double gamma,
                        double xi)
{
    constexpr double kMin = 0.001;
    constexpr double kMax = 1000.0;
    const double gammaDb = gamma <= kMin ? 0.0
        : gamma >= kMax ? 60.0
        : 10.0 * std::log10(gamma / kMin);
    const double xiDb = xi <= kMin ? 0.0
        : xi >= kMax ? 60.0
        : 10.0 * std::log10(xi / kMin);

    const double gammaGrid = 4.0 * gammaDb;
    const double xiGrid = 4.0 * xiDb;
    const int gammaLow = std::min(
        static_cast<int>(gammaGrid), kGammaTableGridSize - 1);
    const int xiLow = std::min(
        static_cast<int>(xiGrid), kGammaTableGridSize - 1);
    const int gammaHigh = std::min(gammaLow + 1,
                                   kGammaTableGridSize - 1);
    const int xiHigh = std::min(xiLow + 1, kGammaTableGridSize - 1);
    const double gammaFraction = gammaGrid - gammaLow;
    const double xiFraction = xiGrid - xiLow;

    const auto value = [&](int xiIndex, int gammaIndex) {
        return tables.values[planeOffset
            + xiIndex * kGammaTableGridSize + gammaIndex];
    };
    const double v00 = value(xiLow, gammaLow);
    const double v01 = value(xiLow, gammaHigh);
    const double v10 = value(xiHigh, gammaLow);
    const double v11 = value(xiHigh, gammaHigh);

    const double low = (1.0 - gammaFraction) * v00
                     + gammaFraction * v01;
    const double high = (1.0 - gammaFraction) * v10
                      + gammaFraction * v11;
    return (1.0 - xiFraction) * low + xiFraction * high;
}

double minimumStatisticsBias(double value)
{
    static constexpr double kDValues[] = {
        1.0, 2.0, 5.0, 8.0, 10.0, 15.0, 20.0, 30.0, 40.0,
        60.0, 80.0, 120.0, 140.0, 160.0, 180.0, 220.0, 260.0, 300.0,
    };
    static constexpr double kMValues[] = {
        0.000, 0.260, 0.480, 0.580, 0.610, 0.668, 0.705, 0.762, 0.800,
        0.841, 0.865, 0.890, 0.900, 0.910, 0.920, 0.930, 0.935, 0.940,
    };

    if (value <= kDValues[0]) {
        return kMValues[0];
    }
    if (value >= kDValues[17]) {
        return kMValues[17];
    }

    int upper = 1;
    while (value > kDValues[upper]) {
        ++upper;
    }
    const double logLow = std::log10(kDValues[upper - 1]);
    const double logHigh = std::log10(kDValues[upper]);
    const double fraction =
        (std::log10(value) - logLow) / (logHigh - logLow);
    return kMValues[upper - 1]
         + fraction * (kMValues[upper] - kMValues[upper - 1]);
}

double scaledSmoothing(double referenceValue,
                       double referenceHop,
                       double referenceRate,
                       int hopSize,
                       int sampleRate)
{
    const double tau = -referenceHop / referenceRate
                     / std::log(referenceValue);
    return std::exp(-static_cast<double>(hopSize) / (sampleRate * tau));
}

double hopScaledSmoothing(double referenceValue, int hopSize, int sampleRate)
{
    return scaledSmoothing(referenceValue, 128.0, 8000.0,
                           hopSize, sampleRate);
}

double expIntegralE1(double x)
{
    if (x <= 0.0) {
        return 1.0e300;
    }
    if (x <= 1.0) {
        double series = 1.0;
        double term = 1.0;
        for (int k = 1; k <= 25; ++k) {
            const double next = static_cast<double>(k + 1);
            term = -term * k * x / (next * next);
            series += term;
            if (std::abs(term) <= std::abs(series) * 1.0e-15) {
                break;
            }
        }
        constexpr double kEulerMascheroni = 0.5772156649015328;
        return -kEulerMascheroni - std::log(x) + x * series;
    }

    const int terms = 20 + static_cast<int>(80.0 / x);
    double continuedFraction = 0.0;
    for (int k = terms; k >= 1; --k) {
        continuedFraction = static_cast<double>(k)
            / (1.0 + k / (x + continuedFraction));
    }
    return std::exp(-x) / (x + continuedFraction);
}

std::string wisdomPathForDirectory(const std::string& directory)
{
    std::string wisdomFile = directory;
    if (!wisdomFile.empty() && wisdomFile.back() != '/' && wisdomFile.back() != '\\')
        wisdomFile += '/';
    wisdomFile += "aethersdr_fftw_wisdom";
    return wisdomFile;
}

std::string wisdomTempPathForDirectory(const std::string& directory)
{
    return wisdomPathForDirectory(directory) + ".tmp";
}

#ifdef HAVE_FFTW3
bool exportWisdomAtomically(const std::string& directory)
{
    const std::string wisdomFile = wisdomPathForDirectory(directory);
    const std::string tempFile = wisdomTempPathForDirectory(directory);

    std::remove(tempFile.c_str());
    if (!fftw_export_wisdom_to_filename(tempFile.c_str())) {
        std::remove(tempFile.c_str());
        qCWarning(lcDsp) << "SpectralNR: failed to export FFTW wisdom to"
                         << QString::fromStdString(tempFile);
        return false;
    }

    std::remove(wisdomFile.c_str());
    if (std::rename(tempFile.c_str(), wisdomFile.c_str()) != 0) {
        qCWarning(lcDsp) << "SpectralNR: failed to install FFTW wisdom at"
                         << QString::fromStdString(wisdomFile);
        std::remove(tempFile.c_str());
        return false;
    }

    return true;
}
#endif

} // namespace

// ─── Construction / Reset ──────────────────────────────────────────────────────

SpectralNR::SpectralNR(int fftSize, int sampleRate, int overlap,
                       bool useLegacyGainMethods)
    : m_fftSize(fftSize)
    , m_overlap(overlap == 4 ? 4 : 2)
    , m_hopSize(fftSize / m_overlap)
    , m_msize(fftSize / 2 + 1)
    , m_sampleRate(sampleRate)
    , m_olaScale(2.0 / static_cast<double>(m_overlap))
    , m_useLegacyGainMethods(useLegacyGainMethods)
{
    // Match WDSP's minimum-statistics geometry and scale every time constant
    // to this instance's actual hop duration. The previous port used fixed
    // frame coefficients and substituted D/V directly for M(D)/M(V), making
    // estimator behavior change with FFT geometry.
    constexpr double kWindowSeconds = 8.0 * 12.0 * 128.0 / 8000.0;
    const double framesPerSec = static_cast<double>(sampleRate) / m_hopSize;
    m_U = 8;
    m_V = std::max(4, static_cast<int>(std::lround(
        kWindowSeconds * sampleRate / (m_U * m_hopSize))));
    m_U = std::max(1, static_cast<int>(std::lround(
        kWindowSeconds * sampleRate / (m_V * m_hopSize))));
    m_D = m_U * m_V;
    m_alphaMax = hopScaledSmoothing(0.96, m_hopSize, sampleRate);
    m_alphaCMin = hopScaledSmoothing(0.7, m_hopSize, sampleRate);
    m_alphaMinMax = hopScaledSmoothing(0.3, m_hopSize, sampleRate);
    m_snrq = -static_cast<double>(m_hopSize) / (0.064 * sampleRate);
    m_betaMax = hopScaledSmoothing(0.8, m_hopSize, sampleRate);
    m_mOfD = minimumStatisticsBias(m_D);
    m_mOfV = minimumStatisticsBias(m_V);

    constexpr double kReferenceSubwindowSeconds = 12.0 * 128.0 / 8000.0;
    const double subwindowSeconds =
        static_cast<double>(m_V * m_hopSize) / sampleRate;
    const double slopeExponent =
        subwindowSeconds / kReferenceSubwindowSeconds;
    m_noiseSlopeMax[0] = std::pow(8.0, slopeExponent);
    m_noiseSlopeMax[1] = std::pow(4.0, slopeExponent);
    m_noiseSlopeMax[2] = std::pow(2.0, slopeExponent);
    m_noiseSlopeMax[3] = std::pow(1.2, slopeExponent);
    m_mmseAlphaPow = hopScaledSmoothing(0.8, m_hopSize, sampleRate);
    m_mmseAlphaPbar = hopScaledSmoothing(0.9, m_hopSize, sampleRate);
    m_nstatEta = scaledSmoothing(
        0.7, 256.0, 20100.0, m_hopSize, sampleRate);
    m_nstatGamma = scaledSmoothing(
        0.998, 256.0, 20100.0, m_hopSize, sampleRate);
    m_nstatBeta = scaledSmoothing(
        0.8, 256.0, 20100.0, m_hopSize, sampleRate);
    m_nstatAlphaD = scaledSmoothing(
        0.85, 256.0, 20100.0, m_hopSize, sampleRate);
    m_nstatAlphaP = scaledSmoothing(
        0.2, 256.0, 20100.0, m_hopSize, sampleRate);
    m_nstatTonalAlpha = std::exp(
        -static_cast<double>(m_hopSize) / (0.25 * sampleRate));
    m_nstatTonalReleaseAlpha = std::exp(
        -static_cast<double>(m_hopSize) / (0.05 * sampleRate));
    m_nstatLowFrequencyBin = static_cast<int>(
        1000.0 / (sampleRate / 2.0) * m_msize);
    m_nstatMidFrequencyBin = static_cast<int>(
        3000.0 / (sampleRate / 2.0) * m_msize);
    m_gainAlpha = hopScaledSmoothing(0.985, m_hopSize, sampleRate);
    m_gainDecreaseSmooth = hopScaledSmoothing(
        0.5, m_hopSize, sampleRate);
    m_rampFrames = std::max(1, static_cast<int>(std::lround(framesPerSec)));
    // Residual-target calibration is an estimator concern, not an audible
    // gain-smoothing preference. Preserve the established 1024/4 default
    // convergence (0.85^93.75) at every supported FFT geometry.
    constexpr double kReferenceGainSmoothing = 0.85;
    constexpr double kReferenceFramesPerSecond = 24000.0 / (1024.0 / 4.0);
    const double residualReferenceInitialWeight = std::pow(
        kReferenceGainSmoothing, kReferenceFramesPerSecond);
    m_residualReferenceAlpha = std::exp(
        std::log(residualReferenceInitialWeight) / m_rampFrames);
    m_commonReferenceAlpha = std::exp(
        -static_cast<double>(m_hopSize) / (2.0 * sampleRate));
    m_commonScaleAlpha = std::exp(
        -static_cast<double>(m_hopSize) / (0.10 * sampleRate));

    // Allocate overlap-add accumulators
    m_inAccum.resize(fftSize * 4, 0.0);
    m_outAccum.resize(fftSize * 4, 0.0);
    m_stereoInAccumL.resize(fftSize * 4, 0.0);
    m_stereoInAccumR.resize(fftSize * 4, 0.0);
    m_stereoOutAccumL.resize(fftSize * 4, 0.0);
    m_stereoOutAccumR.resize(fftSize * 4, 0.0);

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
    // FFTW_MEASURE is used here: fast enough for the NR2 working sizes without
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
    m_osmsNoisePsd.resize(m_msize);
    m_smoothPsd.resize(m_msize);
    m_pMin.resize(m_msize);
    m_pBar.resize(m_msize);
    m_p2Bar.resize(m_msize);
    m_alphaOpt.resize(m_msize);
    m_alphaHat.resize(m_msize);
    m_actMin.resize(m_msize);
    m_actMinSub.resize(m_msize);
    m_kMod.resize(m_msize, 0);
    m_lminFlag.resize(m_msize, 0);
    m_mmseNoisePsd.resize(m_msize);
    m_mmsePbar.resize(m_msize);
    m_nstatPower.resize(m_msize);
    m_nstatPowerMin.resize(m_msize);
    m_nstatSpeechProbability.resize(m_msize);
    m_nstatTonalProbability.resize(m_msize);
    m_nstatTonalIndicator.resize(m_msize, 0);
    m_commonWantedProtected.resize(m_msize, 0);
    m_nstatNoisePsd.resize(m_msize);
    m_commonReferencePsd.resize(m_msize);
    m_residualReferencePsd.resize(m_msize);
    m_residualReferenceGainRatio.resize(m_msize, 1.0);
    m_residualReferenceValid.resize(m_msize, 0);
    m_commonNoiseLike.resize(m_msize, 0);

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
    m_aePrefix.resize(m_msize + 1, 0.0);

    // Decode the read-only Gamma table during filter construction, never on
    // the real-time audio callback when method 2 is first selected.
    static_cast<void>(gammaGainTables());

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

void SpectralNR::setGainMax(float value)
{
    const double safeValue = std::isfinite(value)
        ? std::clamp(static_cast<double>(value), 0.0, 2.0)
        : 1.0;
    m_gainMax.store(safeValue);
}

void SpectralNR::setGainFloor(float value)
{
    const double safeValue = std::isfinite(value)
        ? std::clamp(static_cast<double>(value), 0.0, 1.0)
        : 0.0;
    m_gainFloor.store(safeValue);
}

void SpectralNR::setQspp(float value)
{
    const double safeValue = std::isfinite(value)
        ? std::clamp(static_cast<double>(value), 1.0e-4, 1.0 - 1.0e-4)
        : 0.20;
    m_qSpp.store(safeValue);
}

void SpectralNR::setGainSmooth(float value)
{
    const double safeValue = std::isfinite(value)
        ? std::clamp(static_cast<double>(value), 0.0, 0.9999)
        : 0.85;
    m_gainSmooth.store(safeValue);
}

void SpectralNR::setGainMethod(int method)
{
    m_gainMethod.store(std::clamp(method, 0, 3));
}

void SpectralNR::setNpeMethod(int method)
{
    m_npeMethod.store(std::clamp(method, 0, 2));
}

void SpectralNR::reset()
{
    std::fill(m_inAccum.begin(), m_inAccum.end(), 0.0);
    std::fill(m_outAccum.begin(), m_outAccum.end(), 0.0);
    std::fill(m_stereoInAccumL.begin(), m_stereoInAccumL.end(), 0.0);
    std::fill(m_stereoInAccumR.begin(), m_stereoInAccumR.end(), 0.0);
    std::fill(m_stereoOutAccumL.begin(), m_stereoOutAccumL.end(), 0.0);
    std::fill(m_stereoOutAccumR.begin(), m_stereoOutAccumR.end(), 0.0);
    m_inWritePos = 0;
    m_inReadPos = 0;
    m_samplesAccum = 0;

    // A complete FFT frame is required before its first hop can be finalized.
    // Queue one frame of zero latency padding, then append one finalized hop
    // per processed frame. This keeps output consumption independent of the
    // caller's callback size; without explicit validity tracking, a short
    // callback can read and clear OLA positions before synthesis reaches them.
    m_outWritePos = m_fftSize;
    m_outReadPos = 0;
    m_outputAvailable = m_fftSize;

    // Start with a HIGH noise estimate — gains will be < 1 during convergence,
    // producing gentle suppression rather than amplification spikes.
    // The OSMS tracker will converge downward to the true noise floor in ~2s.
    constexpr double initNoise = 1.0;
    std::fill(m_noisePsd.begin(), m_noisePsd.end(), initNoise);
    std::fill(m_osmsNoisePsd.begin(), m_osmsNoisePsd.end(), initNoise);
    std::fill(m_smoothPsd.begin(), m_smoothPsd.end(), initNoise);
    std::fill(m_pMin.begin(), m_pMin.end(), initNoise);
    std::fill(m_pBar.begin(), m_pBar.end(), initNoise);
    std::fill(m_p2Bar.begin(), m_p2Bar.end(), initNoise * initNoise);
    std::fill(m_alphaOpt.begin(), m_alphaOpt.end(), m_alphaMax);
    std::fill(m_alphaHat.begin(), m_alphaHat.end(), m_alphaMax);
    std::fill(m_actMin.begin(), m_actMin.end(), 1e30);
    std::fill(m_actMinSub.begin(), m_actMinSub.end(), 1e30);
    std::fill(m_kMod.begin(), m_kMod.end(), 0);
    std::fill(m_lminFlag.begin(), m_lminFlag.end(), 0);
    std::fill(m_mmseNoisePsd.begin(), m_mmseNoisePsd.end(), 0.5);
    std::fill(m_mmsePbar.begin(), m_mmsePbar.end(), 0.5);
    std::fill(m_nstatPower.begin(), m_nstatPower.end(), 0.0);
    std::fill(m_nstatPowerMin.begin(), m_nstatPowerMin.end(), 0.0);
    std::fill(m_nstatSpeechProbability.begin(),
              m_nstatSpeechProbability.end(), 0.0);
    std::fill(m_nstatTonalProbability.begin(),
              m_nstatTonalProbability.end(), 0.0);
    std::fill(m_nstatTonalIndicator.begin(),
              m_nstatTonalIndicator.end(), 0);
    std::fill(m_commonWantedProtected.begin(),
              m_commonWantedProtected.end(), 0);
    std::fill(m_nstatNoisePsd.begin(), m_nstatNoisePsd.end(), 0.0);
    std::fill(m_commonReferencePsd.begin(),
              m_commonReferencePsd.end(), 0.0);
    std::fill(m_residualReferencePsd.begin(),
              m_residualReferencePsd.end(), 0.0);
    std::fill(m_residualReferenceGainRatio.begin(),
              m_residualReferenceGainRatio.end(), 1.0);
    std::fill(m_residualReferenceValid.begin(),
              m_residualReferenceValid.end(), 0);
    std::fill(m_commonNoiseLike.begin(), m_commonNoiseLike.end(), 0);

    for (auto& v : m_actMinBuf)
        std::fill(v.begin(), v.end(), 1e30);

    std::fill(m_prevMask.begin(), m_prevMask.end(), 1.0);
    std::fill(m_prevGamma.begin(), m_prevGamma.end(), 1.0);
    std::fill(m_mask.begin(), m_mask.end(), 1.0);
    std::fill(m_smoothMask.begin(), m_smoothMask.end(), 1.0);
    std::fill(m_aeMask.begin(), m_aeMask.end(), 1.0);
    std::fill(m_aePrefix.begin(), m_aePrefix.end(), 0.0);

    m_alphaC = 1.0;
    // WDSP rotates on the first complete frame so the estimator starts from
    // observed audio rather than waiting a full sub-window on its seed value.
    m_subwc = m_V;
    m_ambIdx = 0;
    m_commonReferenceInitialized = false;
    m_commonReferenceReacquiring = false;
    m_commonSilenceRecoveryContext = false;
    m_commonLevelReferenceInitialized = false;
    m_commonLevelReferencePower = 0.0;
    m_commonScaleLog = 0.0;
    m_commonAppliedScale = 1.0;
    m_commonReturnScale = 1.0;
    m_commonDetectedScale = 1.0;
    m_frameCount = 0;
    m_currentWet = 0.0;
}

void SpectralNR::initWindow()
{
    // Periodic Hann window — exact COLA property at 50% and 75% overlap.
    // Applied as sqrt(Hann) at both analysis and synthesis so that
    // w_a[i]*w_s[i] = Hann[i]. m_olaScale normalizes the sum of overlapping
    // Hann windows (1 at 50%, 2 at 75%) back to unity.
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

SpectralNR::WisdomResult SpectralNR::generateWisdom(const std::string& directory,
                                                    WisdomProgressCb progress,
                                                    WisdomCancelCb shouldCancel)
{
#ifdef HAVE_FFTW3
    const auto cancelled = [&shouldCancel]() {
        return shouldCancel && shouldCancel();
    };

    std::remove(wisdomTempPathForDirectory(directory).c_str());

    if (cancelled())
        return WisdomResult::Cancelled;

    if (loadWisdom(directory)) {
        return WisdomResult::Ready;  // wisdom loaded from file — no generation needed
    }
    // If a wisdom file exists but cannot be imported, treat it as stale/partial
    // and remove it before attempting a replacement.
    std::remove(wisdomPathForDirectory(directory).c_str());

    // Try to import Thetis/WDSP wisdom (compatible FFTW3 format)
    // This gives us a head start if Thetis is installed
#ifdef _WIN32
    {
        const QByteArray appData = qgetenv("APPDATA");
        if (!appData.isEmpty()) {
            std::string thetisWisdom = std::string(appData.constData())
                + "\\OpenHPSDR\\Thetis-x64\\wdspWisdom00";
            std::lock_guard<std::mutex> lock(s_fftwMutex);
            if (fftw_import_wisdom_from_filename(thetisWisdom.c_str())) {
                if (cancelled())
                    return WisdomResult::Cancelled;
                // Save as our own so we don't depend on Thetis in future.
                if (!exportWisdomAtomically(directory))
                    return WisdomResult::Failed;
                if (cancelled()) {
                    std::remove(wisdomPathForDirectory(directory).c_str());
                    std::remove(wisdomTempPathForDirectory(directory).c_str());
                    return WisdomResult::Cancelled;
                }
                return WisdomResult::Ready;
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
    if (!cbuf || !rbuf) {
        fftw_free(rbuf);
        fftw_free(cbuf);
        std::remove(wisdomTempPathForDirectory(directory).c_str());
        qCWarning(lcDsp) << "SpectralNR: failed to allocate buffers for FFTW wisdom";
        return WisdomResult::Failed;
    }

    // Count total steps for progress reporting
    // Sizes: 64, 128, 256, ... 262144 = 13 sizes × 4 plan types = 52 steps
    int totalSteps = 0;
    for (int s = 64; s <= maxSize; s *= 2) totalSteps += 4;
    int step = 0;

    for (int psize = 64; psize <= maxSize; psize *= 2) {
        // Lock per-plan to avoid holding the mutex for minutes while still
        // preventing concurrent plan creation (#467)

        // 1. Complex forward
        if (cancelled()) {
            fftw_free(rbuf);
            fftw_free(cbuf);
            std::remove(wisdomTempPathForDirectory(directory).c_str());
            return WisdomResult::Cancelled;
        }
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX FORWARD FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_1d(psize, cbuf, cbuf,
                                            FFTW_FORWARD, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 2. Complex backward (same size)
        if (cancelled()) {
            fftw_free(rbuf);
            fftw_free(cbuf);
            std::remove(wisdomTempPathForDirectory(directory).c_str());
            return WisdomResult::Cancelled;
        }
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX BACKWARD FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_1d(psize, cbuf, cbuf,
                                            FFTW_BACKWARD, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 3. Real-to-complex forward
        if (cancelled()) {
            fftw_free(rbuf);
            fftw_free(cbuf);
            std::remove(wisdomTempPathForDirectory(directory).c_str());
            return WisdomResult::Cancelled;
        }
        if (progress) progress(step, totalSteps,
            "Computing REAL-TO-COMPLEX FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_r2c_1d(psize, rbuf, cbuf, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");

        // 4. Complex-to-real inverse
        if (cancelled()) {
            fftw_free(rbuf);
            fftw_free(cbuf);
            std::remove(wisdomTempPathForDirectory(directory).c_str());
            return WisdomResult::Cancelled;
        }
        if (progress) progress(step, totalSteps,
            "Computing COMPLEX-TO-REAL FFT size " + std::to_string(psize) + "...");
        {   std::lock_guard<std::mutex> lock(s_fftwMutex);
            fftw_plan p = fftw_plan_dft_c2r_1d(psize, cbuf, rbuf, FFTW_PATIENT);
            if (p) fftw_destroy_plan(p);
        }
        if (progress) progress(++step, totalSteps, "");
    }

    if (cancelled()) {
        fftw_free(rbuf);
        fftw_free(cbuf);
        std::remove(wisdomTempPathForDirectory(directory).c_str());
        return WisdomResult::Cancelled;
    }

    bool exported = false;
    {
        std::lock_guard<std::mutex> lock(s_fftwMutex);
        exported = exportWisdomAtomically(directory);
    }
    fftw_free(rbuf);
    fftw_free(cbuf);
    if (cancelled()) {
        std::remove(wisdomPathForDirectory(directory).c_str());
        std::remove(wisdomTempPathForDirectory(directory).c_str());
        return WisdomResult::Cancelled;
    }
    return exported ? WisdomResult::Generated : WisdomResult::Failed;
#else
    (void)directory;
    (void)progress;
    (void)shouldCancel;
    return WisdomResult::Ready;
#endif
}

// ─── Main Processing ───────────────────────────────────────────────────────────

void SpectralNR::process(const float* input, float* output, int numSamples)
{
    if (numSamples <= 0) {
        return;
    }

    if (hasPlanFailed()) {
        std::memmove(output, input, numSamples * sizeof(float));
        return;
    }

    // The overlap-add rings are sized for streaming at the native hop cadence.
    // Larger packet-sized calls can wrap the output ring before the call reads
    // its output, aliasing future synthesis data into the returned samples.
    // Split internally so callers with bursty packet sizes still get the same
    // result as the normal 128-sample Flex RX cadence.
    if (numSamples > m_hopSize) {
        int offset = 0;
        while (offset < numSamples) {
            const int chunk = std::min(m_hopSize, numSamples - offset);
            process(input + offset, output + offset, chunk);
            offset += chunk;
        }
        return;
    }

    const int accSize = static_cast<int>(m_inAccum.size());
    const int outSize = static_cast<int>(m_outAccum.size());

    // Push input samples into accumulator (float32 -> float64)
    for (int i = 0; i < numSamples; ++i) {
        m_inAccum[m_inWritePos] = static_cast<double>(input[i]);
        m_inWritePos = (m_inWritePos + 1) % accSize;
    }
    m_samplesAccum += numSamples;

    // Process complete FFT frames, advancing by the configured hop size.
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
            m_outAccum[idx] += m_olaScale * m_window[i] * m_ifftOut[i];
        }
        m_outWritePos = (m_outWritePos + m_hopSize) % outSize;
        m_outputAvailable += m_hopSize;
    }

    // Read output samples (float64 -> float32), clearing consumed positions
    Q_ASSERT(m_outputAvailable >= numSamples);
    for (int i = 0; i < numSamples; ++i) {
        output[i] = static_cast<float>(m_outAccum[m_outReadPos]);
        m_outAccum[m_outReadPos] = 0.0;
        m_outReadPos = (m_outReadPos + 1) % outSize;
    }
    m_outputAvailable -= numSamples;
}

void SpectralNR::processStereoSharedMask(const float* input, float* output, int numFrames)
{
    if (numFrames <= 0) {
        return;
    }

    if (hasPlanFailed()) {
        std::memmove(output, input, numFrames * 2 * sizeof(float));
        return;
    }

    if (numFrames > m_hopSize) {
        int offset = 0;
        while (offset < numFrames) {
            const int chunk = std::min(m_hopSize, numFrames - offset);
            processStereoSharedMask(input + (2 * offset),
                                    output + (2 * offset),
                                    chunk);
            offset += chunk;
        }
        return;
    }

    const int accSize = static_cast<int>(m_inAccum.size());
    const int outSize = static_cast<int>(m_outAccum.size());

    for (int i = 0; i < numFrames; ++i) {
        const float left = input[2 * i];
        const float right = input[2 * i + 1];
        m_inAccum[m_inWritePos] =
            0.5 * (static_cast<double>(left) + static_cast<double>(right));
        m_stereoInAccumL[m_inWritePos] = static_cast<double>(left);
        m_stereoInAccumR[m_inWritePos] = static_cast<double>(right);
        m_inWritePos = (m_inWritePos + 1) % accSize;
    }
    m_samplesAccum += numFrames;

    while (m_samplesAccum >= m_fftSize) {
        const int frameReadPos = m_inReadPos;

        for (int i = 0; i < m_fftSize; ++i) {
            const int idx = (frameReadPos + i) % accSize;
            m_fftIn[i] = m_window[i] * m_inAccum[idx];
        }

        if (updateMaskFromCurrentFrame()) {
            for (int i = 0; i < m_fftSize; ++i) {
                const int idx = (frameReadPos + i) % accSize;
                m_fftIn[i] = m_window[i] * m_stereoInAccumL[idx];
            }
            synthesizeCurrentFrameWithMask();
            for (int i = 0; i < m_fftSize; ++i) {
                const int idx = (m_outWritePos + i) % outSize;
                m_stereoOutAccumL[idx] +=
                    m_olaScale * m_window[i] * m_ifftOut[i];
            }

            for (int i = 0; i < m_fftSize; ++i) {
                const int idx = (frameReadPos + i) % accSize;
                m_fftIn[i] = m_window[i] * m_stereoInAccumR[idx];
            }
            synthesizeCurrentFrameWithMask();
            for (int i = 0; i < m_fftSize; ++i) {
                const int idx = (m_outWritePos + i) % outSize;
                m_stereoOutAccumR[idx] +=
                    m_olaScale * m_window[i] * m_ifftOut[i];
            }
        }

        m_inReadPos = (m_inReadPos + m_hopSize) % accSize;
        m_samplesAccum -= m_hopSize;
        m_outWritePos = (m_outWritePos + m_hopSize) % outSize;
        m_outputAvailable += m_hopSize;
    }

    Q_ASSERT(m_outputAvailable >= numFrames);
    for (int i = 0; i < numFrames; ++i) {
        output[2 * i] = static_cast<float>(m_stereoOutAccumL[m_outReadPos]);
        output[2 * i + 1] = static_cast<float>(m_stereoOutAccumR[m_outReadPos]);
        m_stereoOutAccumL[m_outReadPos] = 0.0;
        m_stereoOutAccumR[m_outReadPos] = 0.0;
        m_outReadPos = (m_outReadPos + 1) % outSize;
    }
    m_outputAvailable -= numFrames;
}

bool SpectralNR::updateMaskFromCurrentFrame()
{
    // This is per-frame estimator state, not FFTW state. Clear it for both the
    // FFTW and built-in FFT paths so the fallback cannot reuse stale minima.
    std::fill(m_kMod.begin(), m_kMod.end(), 0);
#ifdef HAVE_FFTW3
    if (m_planFailed) return false;  // FFTW plans failed — pass audio through unmodified

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

    // Detect a receiver-AGC common-mode scale before any estimator consumes
    // this periodogram. This is intentionally driven by spectral confidence,
    // not by speech-stop state or a release timer.
    detectCommonModeScale();

    // This order is load-bearing: NSTAT updates the common-mode classification
    // maps consumed by applyCommonModeNoiseEstimate(), even when another NPE
    // method is selected for the actual noise floor.
    estimateNoise();
    applyCommonModeNoiseEstimate();

    // Compute spectral gain mask
    computeGain();

    // Artifact elimination post-processing (smooths gain mask to reduce musical noise)
    if (m_aeFilter.load())
        applyAeFilter();

    // Preserve the pre-AGC residual level for bins that the common-mode
    // detector classified as noise. Estimator rescaling preserves SNR but
    // would otherwise pass proportionally louder post-AGC static.
    const double gainMax = std::max(0.0, m_gainMax.load());
    const double gainFloor = std::clamp(m_gainFloor.load(), 0.0, gainMax);
    // Capture an estimator-level residual target before either the AGC cap or
    // the user gain controls are applied. The current Naturalness/Reduction
    // values are folded into that target below, so changing either control
    // after startup has the same result as selecting it before audio starts.
    updateResidualReference(gainMax, false);
    // Preserve the residual headroom used by the original AGC-release path.
    // The stored PSD is an upper-envelope target captured while the estimator
    // is converging; reproducing it at unity makes the recovered static
    // perceptibly louder than the settled pre-change residual. The continuous
    // detector protects more noise bins than the old release bridge, so keep
    // a 3 dB quiet-side confidence margin on the common-mode residual target.
    constexpr double kCommonResidualHeadroom = 1.0 / std::numbers::sqrt2;
    for (int k = 0; k < m_msize; ++k) {
        double frameGainFloor = gainFloor;
        double frameGainMax = gainMax;
        if (m_commonNoiseLike[k] != 0) {
            const double noisePsd = std::max({m_noisePsd[k], EpsFloor,
                m_commonReferencePsd[k] * m_commonDetectedScale});
            const double referenceGain = std::clamp(
                m_residualReferenceGainRatio[k] * gainMax,
                gainFloor, gainMax);
            const double residualPsd = std::max(
                referenceGain * referenceGain
                    * m_residualReferencePsd[k],
                EpsFloor);
            const double residualHeadroom = m_commonSilenceRecoveryContext
                ? 1.0
                : kCommonResidualHeadroom;
            const double residualGainCap = residualHeadroom
                * std::sqrt(std::clamp(
                    residualPsd / noisePsd, 0.0, 1.0));
            // Naturalness participates in the pre-change referenceGain above;
            // scale that user-selected residual with the receiver level rather
            // than treating the startup value as a permanently absolute floor.
            frameGainFloor = std::min(frameGainFloor, residualGainCap);
            frameGainMax = std::min(gainMax, std::max(
                frameGainFloor, residualGainCap));
            // The decision-directed memory is dimensionless, but it must not
            // keep reopening a bin that the continuous noise classification
            // has just capped. A new speech-like frame clears this naturally.
            m_prevMask[k] = std::min(m_prevMask[k], residualGainCap);
        }
        m_mask[k] = std::clamp(m_mask[k], frameGainFloor, frameGainMax);
    }

    // Temporal gain smoothing — preserve the selected smoothing while a bin is
    // opening for wanted signal, but close it on a shorter, geometry-independent
    // time constant. A symmetric release leaves speech-open bins passing static
    // for several tenths of a second after the speaker stops.
    for (int k = 0; k < m_msize; ++k) {
        const double gs = m_gainSmooth.load();
        double smoothing = gs;
        if (m_mask[k] < m_smoothMask[k]) {
            smoothing = m_commonNoiseLike[k] != 0
                ? 0.0
                : std::min(gs, m_gainDecreaseSmooth);
        }
        m_smoothMask[k] = smoothing * m_smoothMask[k]
                        + (1.0 - smoothing) * m_mask[k];
    }

    // Exact silence can postpone the first usable target until after startup.
    // In that case preserve the established silence-recovery behavior by
    // seeding from the already-capped output mask, not reset-time unity.
    updateResidualReference(gainMax, true);

    // Startup ramp: crossfade from dry (gain=1) to processed over ~1 second
    // to avoid transients while the noise estimator converges.
    ++m_frameCount;
    m_currentWet = (m_frameCount >= m_rampFrames)
        ? 1.0
        : static_cast<double>(m_frameCount) / m_rampFrames;

    return true;
}

void SpectralNR::synthesizeCurrentFrameWithMask()
{
#ifdef HAVE_FFTW3
    if (m_planFailed) return;

    fftw_execute(m_planFwd);
    for (int k = 0; k < m_msize; ++k) {
        m_freqRe[k] = m_fftOut[k][0];
        m_freqIm[k] = m_fftOut[k][1];
    }
#else
    fftForward(m_fftIn.data(), m_freqRe.data(), m_freqIm.data());
#endif

    synthesizeCurrentFrequencyBinsWithMask();
}

void SpectralNR::synthesizeCurrentFrequencyBinsWithMask()
{
    // Apply smoothed gain to frequency bins (with dry/wet blend during startup)
    for (int k = 0; k < m_msize; ++k) {
        double g = m_currentWet * m_smoothMask[k] + (1.0 - m_currentWet) * 1.0;
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

void SpectralNR::processFrame()
{
    if (!updateMaskFromCurrentFrame()) {
        return;
    }
    synthesizeCurrentFrequencyBinsWithMask();
}

// ─── Noise Estimation (dispatches on m_npeMethod) ─────────────────────────────

void SpectralNR::estimateNoise()
{
    // A method can be changed while audio is streaming. Updating only the
    // active estimator leaves the other histories at their reset seeds and
    // creates a level-dependent transient when one is selected later. These
    // passes are O(FFT bins) and small beside the transforms, so keep every
    // estimator current and derive m_noisePsd from the selected result, with
    // recovery-only corroboration below.
    estimateNoiseOsms();
    estimateNoiseMmse();
    estimateNoiseNstat();

    const int method = m_npeMethod.load();
    const std::vector<double>* selected = &m_osmsNoisePsd;
    if (method == 1) {
        selected = &m_mmseNoisePsd;
    } else if (method == 2) {
        selected = &m_nstatNoisePsd;
    }
    for (int k = 0; k < m_msize; ++k) {
        double selectedNoise = std::max((*selected)[k], EpsFloor);
        if (method != 2 && (m_commonReferenceReacquiring
                            || m_commonWantedProtected[k] != 0)) {
            // OSMS/MMSE can eventually absorb a long-held wanted component
            // into their minima. NSTAT stays warm and provides independent
            // per-bin evidence: corroborated excess protects wanted bins,
            // while agreement on stationary energy selects the stronger
            // floor. This stays per-bin because real receiver audio contains
            // speech and noise simultaneously even after a global reference
            // has become valid.
            const double nstatNoise = std::max(
                m_nstatNoisePsd[k], EpsFloor);
            const double nstatRatio = m_nstatPower[k] / nstatNoise;
            const bool wantedLike = isCommonWantedLike(k);
            if (wantedLike) {
                const double wantedConfidence = std::clamp(
                    (nstatRatio - 2.0) / 3.0, 0.0, 1.0);
                const double protectedNoise = std::min(
                    selectedNoise, nstatNoise);
                selectedNoise = (1.0 - wantedConfidence) * selectedNoise
                    + wantedConfidence * protectedNoise;
            } else if (m_commonReferenceReacquiring
                       && nstatRatio <= 2.0) {
                // Both estimators describe stationary energy. Taking the
                // stronger floor prevents the configured slow-minimum method
                // from lagging a newly established colored noise floor; this
                // is estimator fusion based on agreement, not a mode switch.
                selectedNoise = std::max(selectedNoise, nstatNoise);
            }
        }
        m_noisePsd[k] = selectedNoise;
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
        sumSigma2N += m_osmsNoisePsd[k];
    }
    if (sumSigma2N < EpsFloor) sumSigma2N = EpsFloor;
    if (sumLambdaY < EpsFloor) sumLambdaY = EpsFloor;

    // ── Pass 2: Per-bin optimal smoothing + smoothed periodogram ──────
    for (int k = 0; k < m_msize; ++k) {
        double sigma = std::max(m_osmsNoisePsd[k], EpsFloor);
        double f0 = m_smoothPsd[k] / sigma - 1.0;
        m_alphaOpt[k] = 1.0 / (1.0 + f0 * f0);
    }

    double snr = sumPrevP / sumSigma2N;
    double alphaMin = std::min(m_alphaMinMax, std::pow(snr, m_snrq));

    for (int k = 0; k < m_msize; ++k)
        if (m_alphaOpt[k] < alphaMin)
            m_alphaOpt[k] = alphaMin;

    double f1 = sumPrevP / sumLambdaY - 1.0;
    double alphaCtilda = 1.0 / (1.0 + f1 * f1);
    m_alphaC = alphaCSmooth * m_alphaC +
               (1.0 - alphaCSmooth) * std::max(alphaCtilda, m_alphaCMin);

    double f2 = m_alphaMax * m_alphaC;
    for (int k = 0; k < m_msize; ++k) {
        m_alphaHat[k] = f2 * m_alphaOpt[k];
        m_smoothPsd[k] = m_alphaHat[k] * m_smoothPsd[k] +
                         (1.0 - m_alphaHat[k]) * m_lambdaY[k];
    }

    // ── Pass 3: Variance estimation + invQbar accumulation ────────────
    double invQbar = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        double beta = std::min(m_betaMax, m_alphaHat[k] * m_alphaHat[k]);
        m_pBar[k]  = beta * m_pBar[k]  + (1.0 - beta) * m_smoothPsd[k];
        m_p2Bar[k] = beta * m_p2Bar[k] + (1.0 - beta) * m_smoothPsd[k] * m_smoothPsd[k];

        double varHat = m_p2Bar[k] - m_pBar[k] * m_pBar[k];
        double sigma2 = std::max(m_osmsNoisePsd[k], EpsFloor);
        double invQeq = std::min(InvQeqMax, varHat / (2.0 * sigma2 * sigma2));
        invQbar += invQeq;
    }
    invQbar /= static_cast<double>(m_msize);

    // ── Pass 4: Bias correction + minimum tracking (uses final invQbar)
    double bc = 1.0 + 2.12 * std::sqrt(invQbar);

    for (int k = 0; k < m_msize; ++k) {
        double sigma2 = std::max(m_osmsNoisePsd[k], EpsFloor);
        double varHat = m_p2Bar[k] - m_pBar[k] * m_pBar[k];
        double invQeq = std::min(InvQeqMax, varHat / (2.0 * sigma2 * sigma2));
        double Qeq = 1.0 / std::max(invQeq, 1e-10);

        double QeqTilda = (Qeq - 2.0 * m_mOfD) / (1.0 - m_mOfD);
        double bmin = 1.0 + 2.0 * (m_D - 1) / std::max(QeqTilda, 1e-10);

        double QeqTildaSub = (Qeq - 2.0 * m_mOfV) / (1.0 - m_mOfV);
        double bminSub = 1.0 + 2.0 * (m_V - 1) / std::max(QeqTildaSub, 1e-10);

        double f3 = m_smoothPsd[k] * bmin * bc;
        if (f3 < m_actMin[k]) {
            m_actMin[k] = f3;
            m_actMinSub[k] = m_smoothPsd[k] * bminSub * bc;
            m_kMod[k] = 1;
        }
    }

    // ── Sub-window rotation ───────────────────────────────────────────
    // >= (not ==) so a counter that ever advances past m_V still rotates,
    // rather than stalling the minimum tracker permanently.
    if (m_subwc >= m_V) {
        const double noiseSlopeMax = invQbar < 0.03 ? m_noiseSlopeMax[0]
            : invQbar < 0.05 ? m_noiseSlopeMax[1]
            : invQbar < 0.06 ? m_noiseSlopeMax[2]
                             : m_noiseSlopeMax[3];

        for (int k = 0; k < m_msize; ++k) {
            if (m_kMod[k]) {
                m_lminFlag[k] = 0;
            }
            m_actMinBuf[m_ambIdx][k] = m_actMin[k];
            double minVal = 1e30;
            for (int u = 0; u < m_U; ++u)
                minVal = std::min(minVal, m_actMinBuf[u][k]);
            m_pMin[k] = minVal;

            const bool normalRise = m_lminFlag[k]
                && m_actMinSub[k] < noiseSlopeMax * m_pMin[k];
            if (normalRise && m_actMinSub[k] > m_pMin[k]) {
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
                if (m_kMod[k]) {
                    m_lminFlag[k] = 1;
                    m_osmsNoisePsd[k] = std::min(m_actMinSub[k], m_pMin[k]);
                    m_pMin[k] = m_osmsNoisePsd[k];
                }
            }
        }
        ++m_subwc;
    }

    for (int k = 0; k < m_msize; ++k) {
        // estimateNoise() copies the selected estimator into m_noisePsd, so
        // only this estimator's own persistent state is written here.
        m_osmsNoisePsd[k] = m_pMin[k];
    }
}

void SpectralNR::detectCommonModeScale()
{
    if (!m_commonSilenceRecoveryContext) {
        std::fill(m_commonNoiseLike.begin(), m_commonNoiseLike.end(), 0);
    }
    if (!m_commonReferenceInitialized) {
        std::copy(m_lambdaY.begin(), m_lambdaY.end(),
                  m_commonReferencePsd.begin());
        m_commonReferenceInitialized = true;
        return;
    }

    const int firstVoiceBin = std::max(
        1, static_cast<int>(std::ceil(200.0 * m_fftSize / m_sampleRate)));
    const int lastVoiceBin = std::min(
        m_msize - 1,
        static_cast<int>(std::floor(4000.0 * m_fftSize / m_sampleRate)));
    const int radius = std::max(1, static_cast<int>(std::lround(
        100.0 * m_fftSize / m_sampleRate)));
    // A 30 dB receiver gain change is approximately 1000x in power.
    constexpr double kMinCommonPowerRatio = 1.0 / 1024.0;
    constexpr double kMaxCommonPowerRatio = 1024.0;
    // Only retain a quiet-state return marker after an unambiguous >=3 dB
    // power decrease. Smaller motion remains the fine detector's job.
    constexpr double kReturnQuietScaleMax = 0.50;
    constexpr double kReturnStartingScaleMin = 0.90;

    // A receiver passband can leave part of the nominal voice range tens of
    // decibels below its occupied bins. Ratios there are dominated by FFT
    // leakage and must not vote on whether the occupied noise shape moved as
    // one. Derive the evidence floor from the stored reference itself so this
    // remains independent of mode-specific filter limits.
    double peakReferencePower = 0.0;
    for (int k = firstVoiceBin; k <= lastVoiceBin; ++k) {
        const int first = std::max(firstVoiceBin, k - radius);
        const int last = std::min(lastVoiceBin, k + radius);
        double referencePower = 0.0;
        for (int j = first; j <= last; ++j) {
            referencePower += m_commonReferencePsd[j];
        }
        peakReferencePower = std::max(peakReferencePower, referencePower);
    }
    constexpr double kReferenceActivityFraction = 0.01;
    const double referenceActivityFloor = std::max(
        EpsFloor, kReferenceActivityFraction * peakReferencePower);

    int comparableBins = 0;
    double logRatioSum = 0.0;
    double logRatioSquaredSum = 0.0;
    double currentComparablePower = 0.0;
    for (int k = firstVoiceBin; k <= lastVoiceBin; ++k) {
        const int first = std::max(firstVoiceBin, k - radius);
        const int last = std::min(lastVoiceBin, k + radius);
        double currentPower = 0.0;
        double referencePower = 0.0;
        for (int j = first; j <= last; ++j) {
            currentPower += m_lambdaY[j];
            referencePower += m_commonReferencePsd[j];
        }
        if (referencePower < referenceActivityFloor) {
            continue;
        }
        // Keep the ratio finite without clipping a legitimate AGC-T step
        // before the common-mode estimate is formed.
        const double logRatio = std::log(std::clamp(
            currentPower / referencePower,
            kMinCommonPowerRatio, kMaxCommonPowerRatio));
        logRatioSum += logRatio;
        logRatioSquaredSum += logRatio * logRatio;
        currentComparablePower += currentPower;
        ++comparableBins;
    }

    if (m_frameCount < m_rampFrames) {
        if (currentComparablePower > EpsFloor) {
            if (!m_commonLevelReferenceInitialized) {
                m_commonLevelReferencePower = currentComparablePower;
                m_commonLevelReferenceInitialized = true;
            } else {
                // Calibrate the fine-scale statistic from the same arithmetic
                // band-power measure used after startup. A periodogram/reference
                // ratio has a systematic bias at sub-dB resolution; averaging
                // the observed statistic directly avoids treating that bias as
                // a receiver gain change.
                const double sampleWeight = 1.0
                    / static_cast<double>(m_frameCount + 1);
                m_commonLevelReferencePower += sampleWeight
                    * (currentComparablePower - m_commonLevelReferencePower);
            }
        }
        for (int k = 0; k < m_msize; ++k) {
            m_commonReferencePsd[k] = m_commonReferenceAlpha
                * m_commonReferencePsd[k]
                + (1.0 - m_commonReferenceAlpha) * m_lambdaY[k];
        }
        return;
    }

    if (comparableBins == 0) {
        // Exact silence provides no AGC-scale evidence. Follow the first
        // non-zero frame without applying a correction; subsequent frames can
        // establish a common scale, with wanted bins excluded below.
        for (int k = 0; k < m_msize; ++k) {
            m_commonReferencePsd[k] = m_commonReferenceAlpha
                * m_commonReferencePsd[k]
                + (1.0 - m_commonReferenceAlpha) * m_lambdaY[k];
        }
        m_commonReferenceReacquiring = true;
        m_commonSilenceRecoveryContext = true;
        m_commonLevelReferenceInitialized = false;
        m_commonLevelReferencePower = 0.0;
        m_commonScaleLog = 0.0;
        m_commonAppliedScale = 1.0;
        m_commonReturnScale = 1.0;
        m_commonDetectedScale = 1.0;
        return;
    }

    const double meanLogRatio = logRatioSum / comparableBins;
    const double logRatioVariance = std::max(0.0,
        logRatioSquaredSum / comparableBins - meanLogRatio * meanLogRatio);
    const double scale = std::exp(meanLogRatio);
    int coherentBins = 0;
    const double coherenceWidth = 1.10;
    for (int k = firstVoiceBin; k <= lastVoiceBin; ++k) {
        const int first = std::max(firstVoiceBin, k - radius);
        const int last = std::min(lastVoiceBin, k + radius);
        double currentPower = 0.0;
        double referencePower = 0.0;
        for (int j = first; j <= last; ++j) {
            currentPower += m_lambdaY[j];
            referencePower += m_commonReferencePsd[j];
        }
        if (referencePower < referenceActivityFloor) {
            continue;
        }
        const double localLogRatio = std::log(std::clamp(
            currentPower / referencePower,
            kMinCommonPowerRatio, kMaxCommonPowerRatio));
        if (std::abs(localLogRatio - meanLogRatio) <= coherenceWidth) {
            ++coherentBins;
        }
    }

    if (m_commonReferenceReacquiring) {
        double peakTrackedPower = 0.0;
        for (int k = firstVoiceBin; k <= lastVoiceBin; ++k) {
            if (m_commonWantedProtected[k] != 0
                && m_nstatTonalProbability[k] >= 0.50) {
                continue;
            }
            peakTrackedPower = std::max(
                peakTrackedPower, m_nstatPower[k]);
        }
        const double activeBinFloor = 0.01 * peakTrackedPower;
        int activeBins = 0;
        int stationaryBins = 0;
        for (int k = firstVoiceBin; k <= lastVoiceBin; ++k) {
            if (m_commonWantedProtected[k] != 0
                && m_nstatTonalProbability[k] >= 0.50) {
                continue;
            }
            const double trackedPower = m_nstatPower[k];
            if (trackedPower < activeBinFloor) {
                continue;
            }
            ++activeBins;
            if (trackedPower <= 2.0 * std::max(
                    m_nstatNoisePsd[k], EpsFloor)) {
                ++stationaryBins;
            }
        }
        // Judge stationarity against NSTAT's per-bin floor rather than raw
        // spectral flatness. Real receiver filters can color static heavily;
        // its temporal stationarity is still valid evidence, while a new
        // carrier or voice harmonic remains well above the tracked minimum.
        const bool stationaryReference = activeBins > 0
            && stationaryBins >= static_cast<int>(
                std::ceil(0.70 * activeBins));
        const bool coherentReference = coherentBins >= static_cast<int>(
            std::ceil(0.55 * comparableBins))
            && std::sqrt(logRatioVariance) <= coherenceWidth
            && scale > 0.92 && scale < 1.08;
        if (stationaryReference && coherentReference) {
            m_commonReferenceReacquiring = false;
        }
    }

    const bool coherentFrame = coherentBins >= static_cast<int>(
        std::ceil(0.55 * comparableBins))
        && std::sqrt(logRatioVariance) <= coherenceWidth;
    const bool ordinaryContext = !m_commonReferenceReacquiring
        && !m_commonSilenceRecoveryContext;
    const bool commonMode = coherentFrame
        && (scale >= 1.08 || scale <= 0.92);
    bool fineCommonRise = false;
    bool historyReturnRise = false;
    if (commonMode) {
        m_commonDetectedScale = scale;
        if (scale < kReturnQuietScaleMax) {
            m_commonReturnScale = scale;
        } else if (scale > 1.0) {
            m_commonReturnScale = 1.0;
        }
        const double correction = std::clamp(
            scale / m_commonAppliedScale, 0.80, 1.25);
        if (m_commonReferenceReacquiring) {
            // A first non-zero signal after exact silence is ambiguous: its
            // common scale contains both receiver noise and wanted energy.
            // Scale only bins that NSTAT does not independently identify as
            // sustained excess, keeping the noise histories covariant without
            // teaching speech harmonics to the configured estimator.
            for (int k = 0; k < m_msize; ++k) {
                const bool wantedLike = isCommonWantedLike(k);
                m_commonNoiseLike[k] = wantedLike ? 0 : 1;
            }
            scalePowerHistory(correction, &m_commonNoiseLike);
        } else {
            scalePowerHistory(correction);
        }
        m_commonAppliedScale *= correction;
    } else if (scale > 0.92 && scale < 1.08 && coherentFrame) {
        if (ordinaryContext) {
            // Keep a scalar level reference separate from the adaptive spectral
            // shape. The arithmetic band power is unbiased, so a coherent gain
            // change remains measurable even while the per-bin reference learns
            // slow receiver-filter shape changes.
            if (!m_commonLevelReferenceInitialized) {
                m_commonLevelReferencePower = std::max(
                    currentComparablePower, EpsFloor);
                m_commonScaleLog = 0.0;
                m_commonLevelReferenceInitialized = true;
            }
            const double instantaneousScale = std::clamp(
                currentComparablePower
                    / std::max(m_commonLevelReferencePower, EpsFloor),
                kMinCommonPowerRatio, kMaxCommonPowerRatio);
            const double instantaneousLogScale = std::log(
                instantaneousScale);
            m_commonScaleLog = m_commonScaleAlpha * m_commonScaleLog
                + (1.0 - m_commonScaleAlpha) * instantaneousLogScale;
            m_commonDetectedScale = std::exp(m_commonScaleLog);
            // A bare >1 comparison lets ordinary periodogram variance engage
            // the residual controller on stationary noise. Require 0.2 dB of
            // coherent power motion before treating the fine estimate as AGC.
            constexpr double kFineCommonRisePowerRatio = 1.0471285480508996;
            fineCommonRise =
                instantaneousScale > kFineCommonRisePowerRatio;
            if (m_commonReturnScale < kReturnQuietScaleMax
                && instantaneousScale >= kReturnStartingScaleMin) {
                // A down-scale followed by raw scale ~= 1 is a return to the
                // original receiver level, not proof that the quieter residual
                // target vanished. Keep that return visible to the residual
                // controller while the estimators naturally readapt.
                m_commonDetectedScale = std::max(
                    m_commonDetectedScale,
                    1.0 / std::max(m_commonReturnScale, EpsFloor));
                historyReturnRise = true;
            }
        } else {
            m_commonDetectedScale = 1.0;
        }
        const double referenceUpdateScale = fineCommonRise
            ? std::max(m_commonDetectedScale, 1.0)
            : 1.0;
        for (int k = 0; k < m_msize; ++k) {
            const double reference = std::max(m_commonReferencePsd[k], EpsFloor);
            const double normalizedPower = m_lambdaY[k]
                / referenceUpdateScale;
            const double boundedPower = std::clamp(normalizedPower,
                0.25 * reference, 4.0 * reference);
            m_commonReferencePsd[k] = m_commonReferenceAlpha * reference
                + (1.0 - m_commonReferenceAlpha) * boundedPower;
        }
        m_commonAppliedScale = m_commonReferenceAlpha * m_commonAppliedScale
            + (1.0 - m_commonReferenceAlpha);
        if (historyReturnRise) {
            m_commonReturnScale = m_commonReferenceAlpha
                * m_commonReturnScale
                + (1.0 - m_commonReferenceAlpha);
        }
        fineCommonRise = fineCommonRise || historyReturnRise;
    } else {
        m_commonDetectedScale = 1.0;
    }

    if ((!commonMode || scale <= 1.0) && !fineCommonRise) {
        return;
    }
    std::fill(m_commonNoiseLike.begin(), m_commonNoiseLike.end(), 1);
    for (int k = 0; k < m_msize; ++k) {
        const int first = std::max(0, k - radius);
        const int last = std::min(m_msize - 1, k + radius);
        double currentPower = 0.0;
        double referencePower = 0.0;
        for (int j = first; j <= last; ++j) {
            currentPower += m_lambdaY[j];
            referencePower += m_commonReferencePsd[j];
        }
        const double localLogRatio = std::log(std::max(
            currentPower / std::max(referencePower, EpsFloor), EpsFloor));
        if (std::abs(localLogRatio - meanLogRatio) > coherenceWidth) {
            m_commonNoiseLike[k] = 0;
        }
    }
}

bool SpectralNR::isCommonWantedLike(int bin) const
{
    const double nstatNoise = std::max(m_nstatNoisePsd[bin], EpsFloor);
    const bool sustainedEvidence =
        (m_commonWantedProtected[bin] != 0
            && m_nstatTonalProbability[bin] >= 0.10)
        || (m_commonReferenceReacquiring
            && m_nstatSpeechProbability[bin] >= 0.90);
    // A 10x instantaneous excess plus persistent evidence rejects isolated
    // exponential periodogram peaks while retaining a carrier or harmonic.
    return sustainedEvidence
        && m_nstatPower[bin] > 2.0 * nstatNoise
        && m_lambdaY[bin] > 10.0 * nstatNoise;
}

void SpectralNR::applyCommonModeNoiseEstimate()
{
    // A stationary-noise periodogram exceeds its mean sporadically. Requiring
    // 7.4 dB of current excess in addition to NSTAT's sustained evidence
    // reduces isolated random peaks punching holes in the residual cap.
    constexpr double kInstantaneousSpeechExcessRatio = 5.5;
    if (m_commonDetectedScale <= 1.0) {
        return;
    }
    for (int k = 0; k < m_msize; ++k) {
        if (m_commonNoiseLike[k] == 0) {
            continue;
        }
        const double commonNoisePsd = std::max(EpsFloor,
            m_commonReferencePsd[k] * m_commonDetectedScale);
        // NSTAT stays warm for every selected NPE method. Its probability can
        // spike on a common AGC step, so require both sustained confidence and
        // smoothed power that actually exceeds the already-scaled common floor
        // before allowing it to veto the noise classification.
        const bool protectedSpeech = m_nstatSpeechProbability[k] >= 0.90
            && m_nstatPower[k] > 1.10 * commonNoisePsd
            && m_lambdaY[k]
                > kInstantaneousSpeechExcessRatio * commonNoisePsd;
        if (protectedSpeech) {
            m_commonNoiseLike[k] = 0;
            continue;
        }
        m_noisePsd[k] = std::max(m_noisePsd[k], commonNoisePsd);
        m_prevGamma[k] = std::min(
            m_lambdaY[k] / m_noisePsd[k], GammaMax);
        m_prevMask[k] = 0.0;
    }
}

void SpectralNR::scalePowerHistory(
    double ratio, const std::vector<std::uint8_t>* binMask)
{
    const double boundedRatio = std::clamp(ratio, 0.125, 8.0);
    const double ratioSquared = boundedRatio * boundedRatio;
    const auto scalePower = [boundedRatio](double& value) {
        if (value < 1e29) {
            value *= boundedRatio;
        }
    };
    const auto scaleSecondMoment = [ratioSquared](double& value) {
        if (value < 1e29) {
            value *= ratioSquared;
        }
    };
    for (int k = 0; k < m_msize; ++k) {
        if (binMask != nullptr && (*binMask)[k] == 0) {
            continue;
        }
        scalePower(m_noisePsd[k]);
        scalePower(m_osmsNoisePsd[k]);
        scalePower(m_smoothPsd[k]);
        scalePower(m_pMin[k]);
        scalePower(m_pBar[k]);
        scaleSecondMoment(m_p2Bar[k]);
        scalePower(m_actMin[k]);
        scalePower(m_actMinSub[k]);
        scalePower(m_mmseNoisePsd[k]);
        scalePower(m_nstatPower[k]);
        scalePower(m_nstatPowerMin[k]);
        scalePower(m_nstatNoisePsd[k]);
    }
    for (std::vector<double>& minima : m_actMinBuf) {
        for (int k = 0; k < m_msize; ++k) {
            if (binMask == nullptr || (*binMask)[k] != 0) {
                scalePower(minima[k]);
            }
        }
    }
}

void SpectralNR::updateResidualReference(double gainMax, bool afterCap)
{
    for (int k = 0; k < m_msize; ++k) {
        const bool startupSeed = m_frameCount < m_rampFrames && !afterCap;
        const bool lateSeed = m_frameCount >= m_rampFrames && afterCap
            && m_residualReferenceValid[k] == 0;
        if (startupSeed || lateSeed) {
            const double sourceGain = lateSeed ? m_smoothMask[k] : m_mask[k];
            const double gainRatio = gainMax > EpsFloor
                ? std::clamp(sourceGain / gainMax, 0.0, 1.0)
                : 0.0;
            if (lateSeed) {
                m_residualReferenceGainRatio[k] = gainRatio;
            } else {
                m_residualReferenceGainRatio[k] = m_residualReferenceAlpha
                        * m_residualReferenceGainRatio[k]
                    + (1.0 - m_residualReferenceAlpha) * gainRatio;
            }
            // Keep the estimator-level noise PSD separate from gain. The live
            // user floor/cap is deliberately not baked into either value.
            const double noisePsd = std::max(m_noisePsd[k], EpsFloor);
            const double outputPsd = sourceGain * sourceGain * noisePsd;
            if (outputPsd > EpsFloor) {
                m_residualReferencePsd[k] = noisePsd;
                m_residualReferenceValid[k] = 1;
            }
        }
    }
}

// ─── Spectral Gain Computation (dispatches on m_gainMethod) ───────────────────

void SpectralNR::computeGain()
{
    if (m_useLegacyGainMethods) {
        switch (m_gainMethod.load()) {
        case 0:  computeGainWiener();  return;
        case 1:  computeGainLog();     return;
        case 3:  computeGainTrained(); return;
        default: computeGainLinear();  return;
        }
    }

    switch (m_gainMethod.load()) {
    case 0:  computeGainLinear();  return;
    case 1:  computeGainLog();     return;
    case 3:  computeGainTrained(); return;
    default: computeGainGamma();   return;
    }
}

// ─── Original Aether Wiener Approximation (comparison mode method 0) ─────────

void SpectralNR::computeGainWiener()
{
    for (int k = 0; k < m_msize; ++k) {
        const double lambdaD = std::max(m_noisePsd[k], EpsFloor);
        const double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);
        double epsHat = m_gainAlpha * m_prevMask[k] * m_prevMask[k]
                      * m_prevGamma[k]
                      + (1.0 - m_gainAlpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        double gain = epsHat / (1.0 + epsHat);
        const double gmax = m_gainMax.load();
        if (gain > gmax) {
            gain = gmax;
        }
        if (!std::isfinite(gain)) {
            gain = 0.01;
        }

        m_mask[k] = gain;
        m_prevGamma[k] = gamma;
        m_prevMask[k] = gain;
    }
}

// ─── Gaussian Speech Distribution, Linear Amplitude (WDSP method 0) ──────────

void SpectralNR::computeGainLinear()
{
    constexpr double gf1p5 = 0.8862269254527580;  // sqrt(pi) / 2

    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        // A posteriori SNR
        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        // A priori SNR (decision-directed)
        double epsHat = m_gainAlpha * m_prevMask[k] * m_prevMask[k]
                      * m_prevGamma[k]
                      + (1.0 - m_gainAlpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Ephraim-Malah MMSE-LSA
        double ehr = epsHat / (1.0 + epsHat);
        double v = ehr * gamma;

        double gain = gf1p5 * std::sqrt(v) / std::max(gamma, EpsFloor)
                    * ((1.0 + v) * bessI0e(0.5 * v) + v * bessI1e(0.5 * v));

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

// ─── Gamma Speech Distribution (Gamma-prior lookup method 2) ─────────────────
// The two gain surfaces are the compact representation documented above.

void SpectralNR::computeGainGamma()
{
    const GammaGainTables& tables = gammaGainTables();
    if (!tables.valid) {
        computeGainLinear();
        return;
    }

    const double q = std::clamp(m_qSpp.load(), 1e-6, 1.0 - 1e-6);
    const double tablePriorOdds =
        (1.0 - kGammaTableSpeechAbsencePrior)
        / kGammaTableSpeechAbsencePrior;
    const double requestedPriorOdds = (1.0 - q) / q;
    const double softOddsScale = requestedPriorOdds / tablePriorOdds;
    for (int k = 0; k < m_msize; ++k) {
        const double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        const double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        const double epsHat =
            m_gainAlpha * m_prevMask[k] * m_prevMask[k] * m_prevGamma[k]
            + (1.0 - m_gainAlpha) * std::max(gamma - 1.0, EpsFloor);
        const double epsPresence = epsHat / (1.0 - q);

        const double tableSoftWeight = gammaTableLookup(
            tables, kGammaTablePlaneSize, gamma, epsPresence);
        const double scaledSoftNumerator =
            softOddsScale * tableSoftWeight;
        const double softWeight = scaledSoftNumerator
            / (1.0 - tableSoftWeight + scaledSoftNumerator);
        double gain = gammaTableLookup(tables, 0, gamma, epsHat)
                    * softWeight;

        const double gmax = m_gainMax.load();
        if (gain > gmax) {
            gain = gmax;
        }
        if (!std::isfinite(gain)) {
            gain = 0.01;
        }

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

        double epsHat = m_gainAlpha * m_prevMask[k] * m_prevMask[k]
                      * m_prevGamma[k]
                      + (1.0 - m_gainAlpha) * std::max(gamma - 1.0, 0.0);
        epsHat = std::max(epsHat, XiMin);

        // Log-spectral amplitude: G = xi/(1+xi) * exp(0.5 * E1(v))
        // where v = xi*gamma/(1+xi) and E1 is the exponential integral.
        double v = epsHat * gamma / (1.0 + epsHat);
        const double expInt = expIntegralE1(v);

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
// Aether's earlier piecewise experimental curve. It is retained for comparison
// but is not a faithful implementation of WDSP's later method 3, which uses a
// separate zetaHat training-data resource and a two-stage estimator.

void SpectralNR::computeGainTrained()
{
    for (int k = 0; k < m_msize; ++k) {
        double lambdaD = std::max(m_noisePsd[k], EpsFloor);

        double gamma = std::min(m_lambdaY[k] / lambdaD, GammaMax);

        double epsHat = m_gainAlpha * m_prevMask[k] * m_prevMask[k]
                      * m_prevGamma[k]
                      + (1.0 - m_gainAlpha) * std::max(gamma - 1.0, 0.0);
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
// Speech-presence MMSE noise power estimation, matching WDSP LambdaDs.
// Reference: WDSP emnr.c npe_method == 1

void SpectralNR::estimateNoiseMmse()
{
    constexpr double kEpsH1 = 31.622776601683793; // 15 dB
    constexpr double kEpsH1Ratio = kEpsH1 / (1.0 + kEpsH1);

    for (int k = 0; k < m_msize; ++k) {
        const double sigma = std::max(m_mmseNoisePsd[k], EpsFloor);
        double speechPresence = 1.0 / (1.0 + (1.0 + kEpsH1)
            * std::exp(-kEpsH1Ratio * m_lambdaY[k] / sigma));
        m_mmsePbar[k] = m_mmseAlphaPbar * m_mmsePbar[k]
                       + (1.0 - m_mmseAlphaPbar) * speechPresence;
        if (m_mmsePbar[k] > 0.99) {
            speechPresence = std::min(speechPresence, 0.99);
        }
        const double expectedNoise =
            (1.0 - speechPresence) * m_lambdaY[k]
            + speechPresence * sigma;
        m_mmseNoisePsd[k] = m_mmseAlphaPow * sigma
                          + (1.0 - m_mmseAlphaPow) * expectedNoise;
    }
}

// ─── Non-Stationary Noise Estimator (NPE method 2) ───────────────────────────
// Smoothed periodogram/minimum tracker matching WDSP LambdaDl.
// Reference: WDSP emnr.c npe_method == 2

void SpectralNR::estimateNoiseNstat()
{
    const double correction = (1.0 - m_nstatGamma)
                            / (1.0 - m_nstatBeta);

    // A temporal minimum tracker eventually labels a steady carrier or voiced
    // harmonic as noise. Preserve a complementary, smoothed spectral-contrast
    // probability so persistent narrowband structure remains wanted while
    // isolated exponential periodogram peaks decay as non-persistent noise.
    constexpr int kTonalRadius = 4;
    constexpr int kTonalLobeRadius = 1;
    constexpr double kTonalExcessRatio = 6.0;
    for (int k = 0; k < m_msize; ++k) {
        const int first = std::max(0, k - kTonalRadius);
        const int last = std::min(m_msize - 1, k + kTonalRadius);
        double neighborPower = 0.0;
        int neighborCount = 0;
        for (int j = first; j <= last; ++j) {
            if (j != k) {
                neighborPower += m_lambdaY[j];
                ++neighborCount;
            }
        }
        const double neighborMean = neighborPower
            / std::max(1, neighborCount);
        m_nstatTonalIndicator[k] = m_lambdaY[k]
                > kTonalExcessRatio * std::max(neighborMean, EpsFloor)
            ? 1 : 0;
    }
    for (int k = 0; k < m_msize; ++k) {
        const int first = std::max(0, k - kTonalLobeRadius);
        const int last = std::min(m_msize - 1, k + kTonalLobeRadius);
        bool tonalLobe = false;
        for (int j = first; j <= last; ++j) {
            if (m_nstatTonalIndicator[j] != 0) {
                tonalLobe = true;
                break;
            }
        }
        const double tonalIndicator = tonalLobe ? 1.0 : 0.0;
        const double tonalAlpha = tonalLobe
            ? m_nstatTonalAlpha : m_nstatTonalReleaseAlpha;
        m_nstatTonalProbability[k] = tonalAlpha
            * m_nstatTonalProbability[k]
            + (1.0 - tonalAlpha) * tonalIndicator;
        if (m_commonReferenceReacquiring
            && m_nstatTonalProbability[k] >= 0.50) {
            m_commonWantedProtected[k] = 1;
            m_commonNoiseLike[k] = 0;
        } else if (m_commonWantedProtected[k] != 0
                   && m_nstatTonalProbability[k] < 0.10) {
            m_commonWantedProtected[k] = 0;
        }
    }

    for (int k = 0; k < m_msize; ++k) {
        const double oldPower = m_nstatPower[k];
        m_nstatPower[k] = m_nstatEta * oldPower
                        + (1.0 - m_nstatEta) * m_lambdaY[k];
        if (m_nstatPowerMin[k] < m_nstatPower[k]) {
            m_nstatPowerMin[k] = m_nstatGamma * m_nstatPowerMin[k]
                + correction
                * (m_nstatPower[k] - m_nstatBeta * oldPower);
        } else {
            m_nstatPowerMin[k] = m_nstatPower[k];
        }

        const double ratio = m_nstatPower[k]
                           / std::max(m_nstatPowerMin[k], EpsFloor);
        const double threshold = k <= m_nstatLowFrequencyBin ? 2.0
            : k <= m_nstatMidFrequencyBin ? 2.0
                                           : 5.0;
        const double instantaneousRatio = m_lambdaY[k]
            / std::max(m_nstatNoisePsd[k], EpsFloor);
        // After an exact-silence restart, the temporal minimum tracker can
        // retain speech probability after the wanted component has gone.
        // Require current per-bin excess in that recovery context; ordinary
        // running and genuine wanted bins keep the original NSTAT decision.
        const bool currentWantedExcess = !m_commonSilenceRecoveryContext
            || m_commonNoiseLike[k] != 0
            || instantaneousRatio > 5.0;
        const double speechIndicator = ratio > threshold
                && currentWantedExcess
            ? 1.0 : 0.0;
        m_nstatSpeechProbability[k] =
            m_nstatAlphaP * m_nstatSpeechProbability[k]
            + (1.0 - m_nstatAlphaP) * speechIndicator;
        double wantedProbability = m_nstatSpeechProbability[k];
        if (m_commonReferenceReacquiring
            || m_commonWantedProtected[k] != 0) {
            wantedProbability = std::max(
                wantedProbability, m_nstatTonalProbability[k]);
        }
        const double smoothing = m_nstatAlphaD
            + (1.0 - m_nstatAlphaD) * wantedProbability;
        m_nstatNoisePsd[k] = smoothing * m_nstatNoisePsd[k]
                           + (1.0 - smoothing) * m_lambdaY[k];
    }

    if (m_commonSilenceRecoveryContext) {
        bool carrierHeldContext = false;
        bool hasExistingNonTonalAmbiguity = false;
        for (int k = 0; k < m_msize; ++k) {
            if (m_commonNoiseLike[k] != 0) {
                continue;
            }
            if (m_commonWantedProtected[k] != 0
                && m_nstatTonalProbability[k] >= 0.50) {
                carrierHeldContext = true;
            } else {
                hasExistingNonTonalAmbiguity = true;
            }
        }
        carrierHeldContext = carrierHeldContext
            && !hasExistingNonTonalAmbiguity;
        bool hasUnresolvedNonTonalBins = false;
        for (int k = 0; k < m_msize; ++k) {
            const double nstatNoise = std::max(
                m_nstatNoisePsd[k], EpsFloor);
            const double instantaneousRatio = m_lambdaY[k] / nstatNoise;
            if (carrierHeldContext
                && m_commonNoiseLike[k] != 0
                && isCommonWantedLike(k)) {
                // A carrier may keep this transition alive after every other
                // bin has become noise-valid. Make the map reversible so a
                // later reply's corroborated current evidence can reopen its
                // bins instead of remaining under the stale residual cap.
                m_commonNoiseLike[k] = 0;
                hasUnresolvedNonTonalBins = true;
                continue;
            }
            if (m_commonNoiseLike[k] != 0) {
                continue;
            }
            const bool wantedEvidenceGone =
                m_commonWantedProtected[k] == 0
                && m_nstatTonalProbability[k] < 0.10
                && m_nstatSpeechProbability[k] < 0.20
                && instantaneousRatio <= 5.0;
            if (wantedEvidenceGone) {
                // The silence restart made this bin ambiguous during the
                // first common-mode decision. Once the global reference is
                // stationary and independent per-bin evidence has expired,
                // the bin is valid noise for the residual cap again.
                m_commonNoiseLike[k] = 1;
            } else if (m_commonWantedProtected[k] == 0
                       || m_nstatTonalProbability[k] < 0.50) {
                hasUnresolvedNonTonalBins = true;
            }
        }
        if (!hasUnresolvedNonTonalBins
            && !m_commonReferenceReacquiring) {
            // A persistent carrier is a local wanted component, not evidence
            // that every other bin remains globally ambiguous. Once all
            // non-tonal bins are resolved, return to ordinary current-frame
            // decisions so the carrier cannot lock out a later reply.
            m_commonSilenceRecoveryContext = false;
        }
    }
}

// ─── Artifact Elimination Filter ──────────────────────────────────────────────
// Smooths the gain mask across frequency bins to reduce musical noise
// artifacts (isolated spectral peaks in the gain). WDSP adapts the averaging
// width from the frame's post/pre energy ratio. Express the maximum radius in
// Hz here so the 1024/4 test geometry and 256/2 comparison geometry smooth a
// comparable part of the received audio spectrum.
// Reference: WDSP emnr.c aepf() code path

void SpectralNR::applyAeFilter()
{
    constexpr double kZetaThreshold = 0.75;
    constexpr double kMaxHalfWidthHz = 235.0;

    double preEnergy = 0.0;
    double postEnergy = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        preEnergy += m_lambdaY[k];
        postEnergy += m_mask[k] * m_mask[k] * m_lambdaY[k];
    }

    if (preEnergy <= EpsFloor) {
        return;
    }

    const double zeta = std::clamp(postEnergy / preEnergy, 0.0, 1.0);
    const double binWidthHz = static_cast<double>(m_sampleRate) / m_fftSize;
    const int maxRadius = std::max(
        1, static_cast<int>(std::lround(kMaxHalfWidthHz / binWidthHz)));
    const int radius = zeta >= kZetaThreshold
        ? 0
        : static_cast<int>(std::lround(
              maxRadius * (1.0 - zeta / kZetaThreshold)));
    if (radius == 0) {
        return;
    }

    m_aePrefix[0] = 0.0;
    for (int k = 0; k < m_msize; ++k) {
        m_aePrefix[k + 1] = m_aePrefix[k] + m_mask[k];
    }

    for (int k = 0; k < m_msize; ++k) {
        const int localRadius = std::min({radius, k, m_msize - 1 - k});
        const int first = k - localRadius;
        const int last = k + localRadius;
        m_aeMask[k] = (m_aePrefix[last + 1] - m_aePrefix[first])
            / static_cast<double>(last - first + 1);
    }
    std::copy(m_aeMask.begin(), m_aeMask.end(), m_mask.begin());
}

// ─── Modified Bessel Functions ────────────────────────────────────────────────
// Exponentially-scaled polynomial approximations from Abramowitz & Stegun,
// "Handbook of Mathematical Functions" (1964), formulas 9.8.1 and 9.8.2.
// A&S is a U.S. government work and in the public domain.
//
// bessI0e(x) = exp(-|x|) * I0(x),  bessI1e(x) = exp(-|x|) * I1(x).
//
// Scaling eliminates the exp(|x|) overflow present in the unscaled large-x
// branch.  The large-x formula simplifies to 1/sqrt(|x|) * poly — no
// exponential is computed, so no overflow occurs at any finite x (#1507).

double SpectralNR::bessI0e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        return std::exp(-ax) * (1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492
             + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    // Large-x: exp(-ax) * exp(ax)/sqrt(ax) * poly = 1/sqrt(ax) * poly
    double t = 3.75 / ax;
    return (1.0 / std::sqrt(ax))
         * (0.39894228 + t * (0.01328592 + t * (0.00225319
          + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706
          + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
}

double SpectralNR::bessI1e(double x)
{
    double ax = std::abs(x);
    if (ax < 3.75) {
        double t = x / 3.75;
        t *= t;
        double val = std::exp(-ax) * ax * (0.5 + t * (0.87890594 + t * (0.51498869
                   + t * (0.15084934 + t * (0.02658733 + t * (0.00301532
                   + t * 0.00032411))))));
        return x < 0.0 ? -val : val;
    }
    // Large-x: exp(-ax) * exp(ax)/sqrt(ax) * poly = 1/sqrt(ax) * poly
    double t = 3.75 / ax;
    double val = (1.0 / std::sqrt(ax))
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

} // namespace AetherSDR
