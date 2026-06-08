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
#include <cstring>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AetherDemod {

// ── Profile-A tuning constants (from Dire Wolf demod_afsk.c / dsp.c) ─────────

static constexpr float kPrefilterBaud    = 0.155f;  // BPF skirt each side, fraction of baud
static constexpr float kPrefilterLenSym  = 383.f * 1200.f / 44100.f; // ~8.3 symbol-times
static constexpr float kRrcRolloff       = 0.20f;
static constexpr float kRrcWidthSym      = 2.80f;
static constexpr float kAgcFastAttack    = 0.70f;
static constexpr float kAgcSlowDecay     = 0.000090f;
static constexpr float kPllLockedInertia    = 0.74f;
static constexpr float kPllSearchingInertia = 0.50f;
static constexpr int   kMaxFilterTaps    = 2048;

// ── Static members ────────────────────────────────────────────────────────────

float AetherAFSKDemod::s_cosTable[256];
bool  AetherAFSKDemod::s_cosTableReady = false;

void AetherAFSKDemod::buildCosTable() noexcept
{
    for (int j = 0; j < 256; ++j)
        s_cosTable[j] = std::cos(static_cast<float>(j) * 2.0f * float(M_PI) / 256.0f);
    s_cosTableReady = true;
}

// ── Inner helpers ─────────────────────────────────────────────────────────────

void AetherAFSKDemod::pushSample(float val, float* buf, int size) noexcept
{
    std::memmove(buf + 1, buf, static_cast<size_t>(size - 1) * sizeof(float));
    buf[0] = val;
}

float AetherAFSKDemod::convolve(const float* __restrict__ data,
                                 const float* __restrict__ coeffs,
                                 int taps) noexcept
{
    float sum = 0.0f;
    for (int j = 0; j < taps; ++j)
        sum += coeffs[j] * data[j];
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
               : std::sin(float(M_PI) * t) / (float(M_PI) * t);

    float at = a * t;
    float win;
    if (std::fabs(std::fabs(at) - 0.5f) < 0.001f)
        win = float(M_PI) / 4.0f;
    else
        win = std::cos(float(M_PI) * at) / (1.0f - (2.0f * at) * (2.0f * at));

    return sinc * win;
}

void AetherAFSKDemod::buildPrefilter(double fMark, double fSpace,
                                      int bitrate, int sampleRate) noexcept
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
    preCoeffs_.resize(taps);

    for (int j = 0; j < taps; ++j) {
        float d = j - center;
        preCoeffs_[j] = (std::fabs(d) < 1e-6f)
            ? 2.0f * (f2 - f1)
            : std::sin(2.0f * float(M_PI) * f2 * d) / (float(M_PI) * d)
            - std::sin(2.0f * float(M_PI) * f1 * d) / (float(M_PI) * d);
        // Truncated (rectangular) window — matches Dire Wolf profile A.
    }

    // Normalise for unity gain at midband.
    float w = 2.0f * float(M_PI) * 0.5f * (f1 + f2);
    float G = 0.0f;
    for (int j = 0; j < taps; ++j)
        G += 2.0f * preCoeffs_[j] * std::cos((j - center) * w);
    if (G != 0.0f)
        for (auto& c : preCoeffs_) c /= G;

    preTaps_ = taps;
    preBuf_.assign(taps, 0.0f);
}

void AetherAFSKDemod::buildRrcLowpass(int bitrate, int sampleRate) noexcept
{
    float sps  = static_cast<float>(sampleRate) / static_cast<float>(bitrate);
    int   taps = (static_cast<int>(kRrcWidthSym * sps)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    lpCoeffs_.resize(taps);
    for (int k = 0; k < taps; ++k) {
        float t = (k - (taps - 1.0f) * 0.5f) / sps;
        lpCoeffs_[k] = rrcKernel(t, kRrcRolloff);
    }

    // Normalise for unity DC gain.
    float sum = std::accumulate(lpCoeffs_.begin(), lpCoeffs_.end(), 0.0f);
    if (sum != 0.0f)
        for (auto& c : lpCoeffs_) c /= sum;

    lpTaps_ = taps;
    mIBuf_.assign(taps, 0.0f);
    mQBuf_.assign(taps, 0.0f);
    sIBuf_.assign(taps, 0.0f);
    sQBuf_.assign(taps, 0.0f);
}

// ── Constructor ───────────────────────────────────────────────────────────────

AetherAFSKDemod::AetherAFSKDemod(
        double fMark,  double fSpace,  int bitrate,  int sampleRate,
        double /*prefilterBaud*/, double /*filterSymLengths*/,
        double /*sincBw*/,        double /*sincRw*/,
        double /*dfbAlphaMark*/,  double /*dfbAlphaSpace*/,
        double /*pllAlpha*/,
        float  spaceGain)
    : spaceGain_(spaceGain)
{
    if (!s_cosTableReady) buildCosTable();

    buildPrefilter(fMark, fSpace, bitrate, sampleRate);
    buildRrcLowpass(bitrate, sampleRate);

    // Oscillator phase increments — 32-bit unsigned wrapping accumulator.
    mOscDelta_ = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fMark  / sampleRate));
    sOscDelta_ = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fSpace / sampleRate));

    // DPLL: one full 2³² cycle per symbol period.
    pllStep_ = static_cast<int32_t>(
        std::round(4294967296.0 * bitrate / sampleRate));
}

// ── DPLL ─────────────────────────────────────────────────────────────────────

void AetherAFSKDemod::nudgePll(float demodOut, float amplitude) noexcept
{
    prevPll_ = pll_;
    // Unsigned add so wrap-around is well-defined.
    pll_ = static_cast<int32_t>(
        static_cast<uint32_t>(pll_) + static_cast<uint32_t>(pllStep_));

    // Overflow from positive to negative → sample point.
    if (pll_ < 0 && prevPll_ >= 0) {
        float conf = (amplitude > 1e-7f)
                   ? std::min(std::fabs(demodOut) / amplitude, 1.0f)
                   : 0.0f;
        readyBit_  = (demodOut > 0.0f) ? 1u : 0u;
        readyConf_ = conf;
        bitReady_  = true;

        // Sliding-window DCD heuristic.
        bool good = (conf > 0.1f);
        goodHist_ = (goodHist_ << 1) | (good ? 1u : 0u);
        badHist_  = (badHist_  << 1) | (good ? 0u : 1u);
        dcdScore_ = (dcdScore_ << 1);
        int g = std::popcount(goodHist_ & 0xffu);
        int b = std::popcount(badHist_  & 0xffu);
        if (g - b >= 2) dcdScore_ |= 1u;
        int sc = std::popcount(dcdScore_ & 0xffu);
        if (!dataDetect_ && sc >= 6) dataDetect_ = true;
        if ( dataDetect_ && sc <  2) dataDetect_ = false;
    }

    // Nudge phase toward signal on transitions.
    bool d = (demodOut > 0.0f);
    if (d != prevDemod_) {
        float inertia = dataDetect_ ? kPllLockedInertia : kPllSearchingInertia;
        pll_ = static_cast<int32_t>(static_cast<float>(pll_) * inertia);
    }
    prevDemod_ = d;
}

// ── Main demodulator ──────────────────────────────────────────────────────────

bool AetherAFSKDemod::try_demodulate(double sample, demod_result& result) noexcept
{
    // Input is already in [-1, 1] (normalised by the shim).
    float fsam = static_cast<float>(sample);

    // 1. Bandpass prefilter.
    pushSample(fsam, preBuf_.data(), preTaps_);
    fsam = convolve(preBuf_.data(), preCoeffs_.data(), preTaps_);

    // 2. Mix with mark and space local oscillators.
    float mC = fcos(mOscPhase_),  mS = fsin(mOscPhase_);  mOscPhase_ += mOscDelta_;
    float sC = fcos(sOscPhase_),  sS = fsin(sOscPhase_);  sOscPhase_ += sOscDelta_;

    pushSample(fsam * mC, mIBuf_.data(), lpTaps_);
    pushSample(fsam * mS, mQBuf_.data(), lpTaps_);
    pushSample(fsam * sC, sIBuf_.data(), lpTaps_);
    pushSample(fsam * sS, sQBuf_.data(), lpTaps_);

    // 3. RRC lowpass then envelope (amplitude).
    float mI = convolve(mIBuf_.data(), lpCoeffs_.data(), lpTaps_);
    float mQ = convolve(mQBuf_.data(), lpCoeffs_.data(), lpTaps_);
    float sI = convolve(sIBuf_.data(), lpCoeffs_.data(), lpTaps_);
    float sQ = convolve(sQBuf_.data(), lpCoeffs_.data(), lpTaps_);

    float mAmp = std::hypot(mI, mQ);
    float sAmp = std::hypot(sI, sQ);

    // 4. AGC — always run to track peak/valley for amplitude reporting.
    float mNorm = agcStep(mAmp, kAgcFastAttack, kAgcSlowDecay, mPeak_, mValley_);
    float sNorm = agcStep(sAmp, kAgcFastAttack, kAgcSlowDecay, sPeak_, sValley_);

    // 5. Decision — two modes matching Direwolf demod_afsk_process_sample:
    //
    // Single-slicer (spaceGain_==0): AGC-normalised comparison. Both tones
    // scaled to ±0.5 before subtraction, so amplitude imbalance is cancelled.
    // Direwolf passes amplitude=1.0 to nudge_pll in this path.
    //
    // Multi-slicer (spaceGain_!=0): raw-amplitude comparison. Space amplitude
    // is multiplied by spaceGain_ (logarithmically spread 0.5→4.0 across the
    // slicer bank) before subtraction. This directly compensates for VHF FM
    // de-emphasis attenuating the 2200 Hz space tone relative to 1200 Hz mark.
    // Confidence is scaled by the signal envelope, as Direwolf does.
    float demodOut;
    float amplitude;
    if (spaceGain_ == 0.0f) {
        demodOut  = mNorm - sNorm;
        amplitude = 1.0f;
    } else {
        demodOut  = mAmp - sAmp * spaceGain_;
        amplitude = 0.5f * ((mPeak_ - mValley_) + (sPeak_ - sValley_) * spaceGain_);
        if (amplitude < 1e-7f) amplitude = 1.0f;
    }

    // 6. DPLL — fires a bit at each symbol centre.
    nudgePll(demodOut, amplitude);

    if (bitReady_) {
        bitReady_        = false;
        result.bit       = readyBit_;
        result.confidence = static_cast<double>(readyConf_);
        return true;
    }
    return false;
}

bool AetherAFSKDemod::try_demodulate(double sample, uint8_t& bit) noexcept
{
    demod_result r;
    if (try_demodulate(sample, r)) {
        bit = r.bit;
        return true;
    }
    return false;
}

// ── Reset ─────────────────────────────────────────────────────────────────────

void AetherAFSKDemod::reset() noexcept
{
    std::fill(preBuf_.begin(), preBuf_.end(), 0.0f);
    std::fill(mIBuf_.begin(), mIBuf_.end(), 0.0f);
    std::fill(mQBuf_.begin(), mQBuf_.end(), 0.0f);
    std::fill(sIBuf_.begin(), sIBuf_.end(), 0.0f);
    std::fill(sQBuf_.begin(), sQBuf_.end(), 0.0f);

    mOscPhase_ = sOscPhase_ = 0;
    mPeak_ = mValley_ = sPeak_ = sValley_ = 0.0f;
    pll_ = prevPll_ = 0;
    prevDemod_ = dataDetect_ = false;
    goodHist_ = badHist_ = dcdScore_ = 0;
    bitReady_ = false;
    readyBit_ = 0;
    readyConf_ = 0.0f;
}

} // namespace AetherDemod
