// AetherAFSKDemod.h
//
// AFSK 1200 baud demodulator — AetherSDR / AetherAFSKDemod
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Algorithm: Direwolf profile-A
//   BPF → IQ mix (mark/space LOs) → RRC LPF → sqrt → AGC → DPLL
//
// Drop-in replacement for aether_libmodem_core::sinc_corr_afsk_demodulator.
// Used unconditionally for the VHF 1200 baud profile; libmodem handles HF 300.

#pragma once

#include <cstdint>
#include <vector>

// MSVC uses __restrict; GCC/Clang use __restrict__
#ifdef _MSC_VER
#  ifndef __restrict__
#    define __restrict__ __restrict
#  endif
#endif

namespace AetherDemod {

struct demod_result {
    uint8_t bit      = 0;
    double  confidence = 0.0;
};

class AetherAFSKDemod {
public:
    AetherAFSKDemod() = default;

    // Constructor signature matches aether_libmodem_core::sinc_corr_afsk_demodulator.
    // fMark, fSpace, bitrate, sampleRate are used directly.
    // Remaining parameters are accepted for API compatibility; Direwolf's
    // tuned profile-A constants are used internally.
    AetherAFSKDemod(double fMark, double fSpace, int bitrate, int sampleRate,
                    double prefilterBaud, double filterSymLengths,
                    double sincBw,        double sincRw,
                    double dfbAlphaMark,  double dfbAlphaSpace,
                    double pllAlpha,
                    float  spaceGain = 0.0f);

    // Returns true and fills result when a bit is ready (once per symbol period).
    bool try_demodulate(double sample, demod_result& result) noexcept;
    bool try_demodulate(double sample, uint8_t& bit)         noexcept;

    void reset() noexcept;

private:
    // Bandpass prefilter
    std::vector<float> preCoeffs_;
    std::vector<float> preBuf_;
    int preTaps_ {0};

    // RRC lowpass — one set of coefficients, four delay lines (m_I, m_Q, s_I, s_Q)
    std::vector<float> lpCoeffs_;
    std::vector<float> mIBuf_, mQBuf_, sIBuf_, sQBuf_;
    int lpTaps_ {0};

    // Mark/space free-running oscillators (32-bit unsigned phase)
    uint32_t mOscPhase_ {0};
    uint32_t mOscDelta_ {0};
    uint32_t sOscPhase_ {0};
    uint32_t sOscDelta_ {0};

    // Per-tone peak/valley AGC
    float mPeak_   {0.0f};
    float mValley_ {0.0f};
    float sPeak_   {0.0f};
    float sValley_ {0.0f};

    // 0.0f = single-slicer (AGC mode: mNorm−sNorm)
    // non-zero = multi-slicer (raw: mAmp−sAmp*spaceGain_, Direwolf A+ method)
    float   spaceGain_ {0.0f};

    // DPLL state
    int32_t pll_         {0};
    int32_t prevPll_     {0};
    int32_t pllStep_     {0};
    bool    prevDemod_   {false};
    bool    dataDetect_  {false};

    // Simple DCD sliding-window history
    uint32_t goodHist_ {0};
    uint32_t badHist_  {0};
    uint32_t dcdScore_ {0};

    // Output latch — set by nudgePll, consumed by try_demodulate
    bool    bitReady_  {false};
    uint8_t readyBit_  {0};
    float   readyConf_ {0.0f};

    // 256-entry cosine lookup (shared across all instances)
    static float s_cosTable[256];
    static bool  s_cosTableReady;
    static void  buildCosTable() noexcept;

    static inline float fcos(uint32_t phase) noexcept
        { return s_cosTable[(phase >> 24) & 0xffu]; }
    static inline float fsin(uint32_t phase) noexcept
        { return s_cosTable[((phase >> 24) - 64u) & 0xffu]; }

    void buildPrefilter(double fMark, double fSpace, int bitrate, int sampleRate) noexcept;
    void buildRrcLowpass(int bitrate, int sampleRate) noexcept;

    static float  convolve  (const float* __restrict__ data,
                              const float* __restrict__ coeffs, int taps) noexcept;
    static void   pushSample(float val, float* buf, int size) noexcept;
    static float  agcStep   (float in, float fast, float slow,
                              float& peak, float& valley) noexcept;

    void nudgePll(float demodOut, float amplitude) noexcept;
};

// API alias — makes AetherDemod a drop-in for aether_libmodem_core's class.
using sinc_corr_afsk_demodulator = AetherAFSKDemod;

} // namespace AetherDemod
