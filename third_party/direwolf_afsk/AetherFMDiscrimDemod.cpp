// AetherFMDiscrimDemod.cpp
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Reference: src/demod_afsk.c (profile B) and src/dsp.c from Dire Wolf.

#include "AetherFMDiscrimDemod.h"

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

// ── Profile-B tuning constants (from Dire Wolf demod_afsk.c) ─────────────────

static constexpr float kPrefilterBaud      = 0.19f;   // BPF skirt each side
static constexpr float kPrefilterLenSym    = 8.163f;  // filter length in symbols
static constexpr float kRrcRolloff         = 0.40f;   // wider than profile-A (0.20)
static constexpr float kRrcWidthSym        = 2.00f;   // shorter than profile-A (2.80)
static constexpr float kPllLockedInertia   = 0.74f;
static constexpr float kPllSearchingInertia = 0.50f;
static constexpr int   kMaxFilterTaps      = 2048;

// ── Static members ────────────────────────────────────────────────────────────

float AetherFMDiscrimDemod::s_cosTable[256];
bool  AetherFMDiscrimDemod::s_cosTableReady = false;

void AetherFMDiscrimDemod::buildCosTable() noexcept
{
    for (int j = 0; j < 256; ++j)
        s_cosTable[j] = std::cos(static_cast<float>(j) * 2.0f * float(M_PI) / 256.0f);
    s_cosTableReady = true;
}

// ── Inner helpers ─────────────────────────────────────────────────────────────

void AetherFMDiscrimDemod::pushSample(float val, float* buf, int size) noexcept
{
    std::memmove(buf + 1, buf, static_cast<size_t>(size - 1) * sizeof(float));
    buf[0] = val;
}

float AetherFMDiscrimDemod::convolve(const float* __restrict__ data,
                                      const float* __restrict__ coeffs,
                                      int taps) noexcept
{
    float sum = 0.0f;
    for (int j = 0; j < taps; ++j)
        sum += coeffs[j] * data[j];
    return sum;
}

// ── Filter design ─────────────────────────────────────────────────────────────

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

void AetherFMDiscrimDemod::buildPrefilter(double fMark, double fSpace,
                                           int bitrate, int sampleRate) noexcept
{
    int taps = (static_cast<int>(kPrefilterLenSym * sampleRate / bitrate)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

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
    }

    float w = 2.0f * float(M_PI) * 0.5f * (f1 + f2);
    float G = 0.0f;
    for (int j = 0; j < taps; ++j)
        G += 2.0f * preCoeffs_[j] * std::cos((j - center) * w);
    if (G != 0.0f)
        for (auto& c : preCoeffs_) c /= G;

    preTaps_ = taps;
    preBuf_.assign(taps, 0.0f);
}

void AetherFMDiscrimDemod::buildRrcLowpass(int bitrate, int sampleRate) noexcept
{
    float sps  = static_cast<float>(sampleRate) / static_cast<float>(bitrate);
    int   taps = (static_cast<int>(kRrcWidthSym * sps)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    lpCoeffs_.resize(taps);
    for (int k = 0; k < taps; ++k) {
        float t = (k - (taps - 1.0f) * 0.5f) / sps;
        lpCoeffs_[k] = rrcKernel(t, kRrcRolloff);
    }

    float sum = std::accumulate(lpCoeffs_.begin(), lpCoeffs_.end(), 0.0f);
    if (sum != 0.0f)
        for (auto& c : lpCoeffs_) c /= sum;

    lpTaps_ = taps;
    cIBuf_.assign(taps, 0.0f);
    cQBuf_.assign(taps, 0.0f);
}

// ── Constructor ───────────────────────────────────────────────────────────────

AetherFMDiscrimDemod::AetherFMDiscrimDemod(
        double fMark, double fSpace, int bitrate, int sampleRate,
        float sliceOffset)
    : sliceOffset_(sliceOffset)
{
    if (!s_cosTableReady) buildCosTable();

    buildPrefilter(fMark, fSpace, bitrate, sampleRate);
    buildRrcLowpass(bitrate, sampleRate);

    // Center-frequency oscillator: (fMark + fSpace) / 2
    const double fCenter = 0.5 * (fMark + fSpace);
    cOscDelta_ = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fCenter / sampleRate));

    pllStep_ = static_cast<int32_t>(
        std::round(4294967296.0 * bitrate / sampleRate));

    // Scale factor: radians/sample → ±1.0 for expected mark/space tones.
    // At the mark frequency the instantaneous rate is fMark*2π/sampleRate;
    // at space it's fSpace*2π/sampleRate.  The midpoint is fCenter*2π/sampleRate
    // and the expected half-deviation is |fMark-fSpace|/2 * 2π/sampleRate.
    normalizeRpsam_ = static_cast<float>(
        1.0 / (0.5 * std::abs(fMark - fSpace) * 2.0 * M_PI / sampleRate));
}

// ── DPLL ─────────────────────────────────────────────────────────────────────

void AetherFMDiscrimDemod::nudgePll(float demodOut) noexcept
{
    prevPll_ = pll_;
    pll_ = static_cast<int32_t>(
        static_cast<uint32_t>(pll_) + static_cast<uint32_t>(pllStep_));

    if (pll_ < 0 && prevPll_ >= 0) {
        // Confidence: how far the discriminator is from the decision threshold.
        // Amplitude is always 1.0 for profile B (self-normalizing discriminator).
        float conf = std::min(std::fabs(demodOut), 1.0f);
        readyBit_  = (demodOut > 0.0f) ? 1u : 0u;
        readyConf_ = conf;
        bitReady_  = true;

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

    bool d = (demodOut > 0.0f);
    if (d != prevDemod_) {
        float inertia = dataDetect_ ? kPllLockedInertia : kPllSearchingInertia;
        pll_ = static_cast<int32_t>(static_cast<float>(pll_) * inertia);
    }
    prevDemod_ = d;
}

// ── Main demodulator ──────────────────────────────────────────────────────────

bool AetherFMDiscrimDemod::try_demodulate(double sample, demod_result& result) noexcept
{
    float fsam = static_cast<float>(sample);

    // 1. Bandpass prefilter.
    pushSample(fsam, preBuf_.data(), preTaps_);
    fsam = convolve(preBuf_.data(), preCoeffs_.data(), preTaps_);

    // 2. Mix with center-frequency oscillator.
    float cC = fcos(cOscPhase_),  cS = fsin(cOscPhase_);  cOscPhase_ += cOscDelta_;

    pushSample(fsam * cC, cIBuf_.data(), lpTaps_);
    pushSample(fsam * cS, cQBuf_.data(), lpTaps_);

    // 3. RRC lowpass.
    float cI = convolve(cIBuf_.data(), lpCoeffs_.data(), lpTaps_);
    float cQ = convolve(cQBuf_.data(), lpCoeffs_.data(), lpTaps_);

    // 4. Instantaneous phase via atan2.
    float phase = std::atan2(cQ, cI);

    // 5. Differentiate phase → frequency deviation; handle ±π wrap.
    float rate = phase - prevPhase_;
    if (rate >  float(M_PI)) rate -= 2.0f * float(M_PI);
    if (rate < -float(M_PI)) rate += 2.0f * float(M_PI);
    prevPhase_ = phase;

    // 6. Normalize so expected mark deviation ≈ +1, space ≈ −1.
    //    sliceOffset_ shifts the decision threshold (B+ multi-slicer).
    float demodOut = rate * normalizeRpsam_ + sliceOffset_;

    // 7. DPLL — fires a bit at each symbol centre.
    nudgePll(demodOut);

    if (bitReady_) {
        bitReady_         = false;
        result.bit        = readyBit_;
        result.confidence = static_cast<double>(readyConf_);
        return true;
    }
    return false;
}

bool AetherFMDiscrimDemod::try_demodulate(double sample, uint8_t& bit) noexcept
{
    demod_result r;
    if (try_demodulate(sample, r)) {
        bit = r.bit;
        return true;
    }
    return false;
}

// ── Reset ─────────────────────────────────────────────────────────────────────

void AetherFMDiscrimDemod::reset() noexcept
{
    std::fill(preBuf_.begin(), preBuf_.end(), 0.0f);
    std::fill(cIBuf_.begin(), cIBuf_.end(), 0.0f);
    std::fill(cQBuf_.begin(), cQBuf_.end(), 0.0f);

    cOscPhase_ = 0;
    prevPhase_ = 0.0f;
    pll_ = prevPll_ = 0;
    prevDemod_ = dataDetect_ = false;
    goodHist_ = badHist_ = dcdScore_ = 0;
    bitReady_ = false;
    readyBit_ = 0;
    readyConf_ = 0.0f;
}

} // namespace AetherDemod
