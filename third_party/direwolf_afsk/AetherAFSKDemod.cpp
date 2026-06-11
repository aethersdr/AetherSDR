// AetherAFSKDemod.cpp
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Reference: src/demod_afsk.c (profile A) and src/dsp.c from Dire Wolf.

#include "AetherAFSKDemod.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <numbers>
#include <numeric>

namespace AetherDemod {

// ── Profile-A tuning constants (from Dire Wolf demod_afsk.c / dsp.c) ─────────

static constexpr float kPrefilterBaud    = 0.155f;  // BPF skirt each side, fraction of baud
static constexpr float kPrefilterLenSym  = 383.f * 1200.f / 44100.f; // ~10.4 symbol-times
static constexpr float kRrcRolloff       = 0.20f;
static constexpr float kRrcWidthSym      = 2.80f;
static constexpr float kAgcFastAttack    = 0.70f;
static constexpr float kAgcSlowDecay     = 0.000090f;
static constexpr float kPllLockedInertia    = 0.74f;
static constexpr float kPllSearchingInertia = 0.50f;
static constexpr int   kMaxFilterTaps    = 2048;

// ── Static members ────────────────────────────────────────────────────────────

float             AetherAFSKDemod::s_cosTable[256];
std::once_flag    AetherAFSKDemod::s_cosOnce;

void AetherAFSKDemod::buildCosTable() noexcept
{
    for (int j = 0; j < 256; ++j)
        s_cosTable[j] = std::cos(static_cast<float>(j) * 2.0f * std::numbers::pi_v<float> / 256.0f);
}

// ── Inner helpers ─────────────────────────────────────────────────────────────

void AetherAFSKDemod::pushSample(float val, float* buf, int& wrPos, int taps) noexcept
{
    buf[wrPos] = val;
    if (++wrPos >= taps) wrPos = 0;
}

void AetherAFSKDemod::pushLP(float mI, float mQ, float sI, float sQ) noexcept
{
    m_mIBuf[m_lpBufPos] = mI;
    m_mQBuf[m_lpBufPos] = mQ;
    m_sIBuf[m_lpBufPos] = sI;
    m_sQBuf[m_lpBufPos] = sQ;
    if (++m_lpBufPos >= m_lpTaps) m_lpBufPos = 0;
}

float AetherAFSKDemod::convolve(const float* buf, int wrPos, const float* coeffs, int taps) noexcept
{
    // Ring-buffer dot product: coeffs[0] matches the newest sample (wrPos-1), etc.
    float sum = 0.0f;
    int idx = wrPos - 1;
    if (idx < 0) idx += taps;
    for (int j = 0; j < taps; ++j) {
        sum += coeffs[j] * buf[idx];
        if (--idx < 0) idx += taps;
    }
    return sum;
}

// Peak/valley AGC — output settles to ≈ [-0.5, +0.5].
float AetherAFSKDemod::agcStep(float in, float fast, float slow,
                                float& peak, float& valley) noexcept
{
    peak   = (in >= peak)   ? in * fast + peak   * (1.0f - fast)
                            : in * slow + peak   * (1.0f - slow);
    valley = (in <= valley) ? in * fast + valley * (1.0f - fast)
                            : in * slow + valley * (1.0f - slow);

    float x = std::max(valley, std::min(peak, in));
    if (peak > valley)
        return (x - 0.5f * (peak + valley)) / (peak - valley);
    return 0.0f;
}

// ── Filter design ─────────────────────────────────────────────────────────────

// RRC kernel: sinc × raised-cosine window (from Dire Wolf dsp.c).
static float rrcKernel(float t, float a) noexcept
{
    float sinc = (std::fabs(t) < 0.001f)
               ? 1.0f
               : std::sin(std::numbers::pi_v<float> * t) / (std::numbers::pi_v<float> * t);

    float at = a * t;
    float win;
    if (std::fabs(std::fabs(at) - 0.5f) < 0.001f)
        win = std::numbers::pi_v<float> / 4.0f;
    else
        win = std::cos(std::numbers::pi_v<float> * at) / (1.0f - (2.0f * at) * (2.0f * at));

    return sinc * win;
}

void AetherAFSKDemod::buildPrefilter(double fMark, double fSpace,
                                      int bitrate, int sampleRate)
{
    int taps = (static_cast<int>(kPrefilterLenSym * sampleRate / bitrate)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    // Normalised cutoff frequencies
    float f1 = static_cast<float>(
        (std::min(fMark, fSpace) - kPrefilterBaud * bitrate) / sampleRate);
    float f2 = static_cast<float>(
        (std::max(fMark, fSpace) + kPrefilterBaud * bitrate) / sampleRate);
    f1 = std::max(f1, 1.0f / sampleRate);
    f2 = std::min(f2, 0.499f);

    float center = 0.5f * (taps - 1);
    m_preCoeffs.resize(taps);

    for (int j = 0; j < taps; ++j) {
        float d = j - center;
        m_preCoeffs[j] = (std::fabs(d) < 1e-6f)
            ? 2.0f * (f2 - f1)
            : std::sin(2.0f * std::numbers::pi_v<float> * f2 * d) / (std::numbers::pi_v<float> * d)
            - std::sin(2.0f * std::numbers::pi_v<float> * f1 * d) / (std::numbers::pi_v<float> * d);
        // Truncated (rectangular) window — matches Dire Wolf profile A.
    }

    // Normalise for unity gain at midband.
    float w = 2.0f * std::numbers::pi_v<float> * 0.5f * (f1 + f2);
    float G = 0.0f;
    for (int j = 0; j < taps; ++j)
        G += 2.0f * m_preCoeffs[j] * std::cos((j - center) * w);
    if (G != 0.0f)
        for (auto& c : m_preCoeffs) c /= G;

    m_preTaps = taps;
    m_preBuf.assign(taps, 0.0f);
    m_preBufPos = 0;
}

void AetherAFSKDemod::buildRrcLowpass(int bitrate, int sampleRate)
{
    float sps  = static_cast<float>(sampleRate) / static_cast<float>(bitrate);
    int   taps = (static_cast<int>(kRrcWidthSym * sps)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    m_lpCoeffs.resize(taps);
    for (int k = 0; k < taps; ++k) {
        float t = (k - (taps - 1.0f) * 0.5f) / sps;
        m_lpCoeffs[k] = rrcKernel(t, kRrcRolloff);
    }

    // Normalise for unity DC gain.
    float sum = std::accumulate(m_lpCoeffs.begin(), m_lpCoeffs.end(), 0.0f);
    if (sum != 0.0f)
        for (auto& c : m_lpCoeffs) c /= sum;

    m_lpTaps = taps;
    m_mIBuf.assign(taps, 0.0f);
    m_mQBuf.assign(taps, 0.0f);
    m_sIBuf.assign(taps, 0.0f);
    m_sQBuf.assign(taps, 0.0f);
    m_lpBufPos = 0;
}

// ── Constructor ───────────────────────────────────────────────────────────────

AetherAFSKDemod::AetherAFSKDemod(
        double fMark,  double fSpace,  int bitrate,  int sampleRate,
        double /*prefilterBaud*/, double /*filterSymLengths*/,
        double /*sincBw*/,        double /*sincRw*/,
        double /*dfbAlphaMark*/,  double /*dfbAlphaSpace*/,
        double /*pllAlpha*/,
        float  spaceGain)
    : m_spaceGain(spaceGain)
{
    std::call_once(s_cosOnce, buildCosTable);

    buildPrefilter(fMark, fSpace, bitrate, sampleRate);
    buildRrcLowpass(bitrate, sampleRate);

    // Oscillator phase increments — 32-bit unsigned wrapping accumulator.
    m_mOscDelta = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fMark  / sampleRate));
    m_sOscDelta = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fSpace / sampleRate));

    // DPLL: one full 2³² cycle per symbol period.
    m_pllStep = static_cast<int32_t>(
        std::round(4294967296.0 * bitrate / sampleRate));
}

// ── DPLL ─────────────────────────────────────────────────────────────────────

void AetherAFSKDemod::nudgePll(float demodOut, float amplitude) noexcept
{
    m_prevPll = m_pll;
    // Unsigned add so wrap-around is well-defined.
    m_pll = static_cast<int32_t>(
        static_cast<uint32_t>(m_pll) + static_cast<uint32_t>(m_pllStep));

    // Overflow from positive to negative → sample point.
    if (m_pll < 0 && m_prevPll >= 0) {
        float conf = (amplitude > 1e-7f)
                   ? std::min(std::fabs(demodOut) / amplitude, 1.0f)
                   : 0.0f;
        m_readyBit  = (demodOut > 0.0f) ? 1u : 0u;
        m_readyConf = conf;
        m_bitReady  = true;

        // Sliding-window DCD heuristic.
        bool good = (conf > 0.1f);
        m_goodHist = (m_goodHist << 1) | (good ? 1u : 0u);
        m_dcdScore = (m_dcdScore << 1);
        // m_goodHist and its complement always sum to 8 in the low byte; g-b>=2 ⟺ g>=5.
        if (std::popcount(m_goodHist & 0xffu) >= 5) m_dcdScore |= 1u;
        int sc = std::popcount(m_dcdScore & 0xffu);
        if (!m_dataDetect && sc >= 6) m_dataDetect = true;
        if ( m_dataDetect && sc <  2) m_dataDetect = false;
    }

    // Nudge phase toward signal on transitions.
    bool d = (demodOut > 0.0f);
    if (d != m_prevDemod) {
        float inertia = m_dataDetect ? kPllLockedInertia : kPllSearchingInertia;
        m_pll = static_cast<int32_t>(static_cast<float>(m_pll) * inertia);
    }
    m_prevDemod = d;
}

// ── Main demodulator ──────────────────────────────────────────────────────────

bool AetherAFSKDemod::try_demodulate(double sample, demod_result& result) noexcept
{
    // Input is already in [-1, 1] (normalised by the shim).
    float fsam = static_cast<float>(sample);

    // 1. Bandpass prefilter.
    pushSample(fsam, m_preBuf.data(), m_preBufPos, m_preTaps);
    fsam = convolve(m_preBuf.data(), m_preBufPos, m_preCoeffs.data(), m_preTaps);

    // 2. Mix with mark and space local oscillators.
    float mC = fcos(m_mOscPhase),  mS = fsin(m_mOscPhase);  m_mOscPhase += m_mOscDelta;
    float sC = fcos(m_sOscPhase),  sS = fsin(m_sOscPhase);  m_sOscPhase += m_sOscDelta;

    pushLP(fsam * mC, fsam * mS, fsam * sC, fsam * sS);

    // 3. RRC lowpass then envelope (amplitude).
    float mI = convolve(m_mIBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);
    float mQ = convolve(m_mQBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);
    float sI = convolve(m_sIBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);
    float sQ = convolve(m_sQBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);

    float mAmp = std::hypot(mI, mQ);
    float sAmp = std::hypot(sI, sQ);

    // 4. AGC — always run to track peak/valley for amplitude reporting.
    float mNorm = agcStep(mAmp, kAgcFastAttack, kAgcSlowDecay, m_mPeak, m_mValley);
    float sNorm = agcStep(sAmp, kAgcFastAttack, kAgcSlowDecay, m_sPeak, m_sValley);

    // 5. Decision — two modes matching Direwolf demod_afsk_process_sample:
    //
    // Single-slicer (m_spaceGain==0): AGC-normalised comparison. Both tones
    // scaled to ±0.5 before subtraction, so amplitude imbalance is cancelled.
    // Direwolf passes amplitude=1.0 to nudge_pll in this path.
    //
    // Multi-slicer (m_spaceGain!=0): raw-amplitude comparison. Space amplitude
    // is multiplied by m_spaceGain (logarithmically spread 0.5→4.0 across the
    // slicer bank) before subtraction. This directly compensates for VHF FM
    // de-emphasis attenuating the 2200 Hz space tone relative to 1200 Hz mark.
    // Confidence is scaled by the signal envelope, as Direwolf does.
    float demodOut;
    float amplitude;
    if (m_spaceGain == 0.0f) {
        demodOut  = mNorm - sNorm;
        amplitude = 1.0f;
    } else {
        demodOut  = mAmp - sAmp * m_spaceGain;
        amplitude = 0.5f * ((m_mPeak - m_mValley) + (m_sPeak - m_sValley) * m_spaceGain);
        if (amplitude < 1e-7f) amplitude = 1.0f;
    }

    // 6. DPLL — fires a bit at each symbol centre.
    nudgePll(demodOut, amplitude);

    if (m_bitReady) {
        m_bitReady        = false;
        result.bit       = m_readyBit;
        result.confidence = static_cast<double>(m_readyConf);
        return true;
    }
    return false;
}


// ── Reset ─────────────────────────────────────────────────────────────────────

void AetherAFSKDemod::reset() noexcept
{
    std::fill(m_preBuf.begin(), m_preBuf.end(), 0.0f);
    std::fill(m_mIBuf.begin(), m_mIBuf.end(), 0.0f);
    std::fill(m_mQBuf.begin(), m_mQBuf.end(), 0.0f);
    std::fill(m_sIBuf.begin(), m_sIBuf.end(), 0.0f);
    std::fill(m_sQBuf.begin(), m_sQBuf.end(), 0.0f);

    m_mOscPhase = m_sOscPhase = 0;
    m_mPeak = m_mValley = m_sPeak = m_sValley = 0.0f;
    m_pll = m_prevPll = 0;
    m_prevDemod = m_dataDetect = false;
    m_goodHist = m_dcdScore = 0;
    m_preBufPos = m_lpBufPos = 0;
    m_bitReady = false;
    m_readyBit = 0;
    m_readyConf = 0.0f;
}

} // namespace AetherDemod
