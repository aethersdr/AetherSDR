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
#include <mutex>
#include <vector>


namespace AetherDemod {

struct demod_result {
    uint8_t bit      = 0;
    double  confidence = 0.0;
};

class AetherAFSKDemod {
public:
    AetherAFSKDemod() = delete;

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

    void reset() noexcept;

private:
    // Bandpass prefilter
    std::vector<float> m_preCoeffs;
    std::vector<float> m_preBuf;
    int m_preTaps {0};
    int m_preBufPos {0};

    // RRC lowpass — one set of coefficients, four delay lines (m_I, m_Q, s_I, s_Q)
    std::vector<float> m_lpCoeffs;
    std::vector<float> m_mIBuf, m_mQBuf, m_sIBuf, m_sQBuf;
    int m_lpTaps {0};
    int m_lpBufPos {0};

    // Mark/space free-running oscillators (32-bit unsigned phase)
    uint32_t m_mOscPhase {0};
    uint32_t m_mOscDelta {0};
    uint32_t m_sOscPhase {0};
    uint32_t m_sOscDelta {0};

    // Per-tone peak/valley AGC
    float m_mPeak   {0.0f};
    float m_mValley {0.0f};
    float m_sPeak   {0.0f};
    float m_sValley {0.0f};

    // 0.0f = single-slicer (AGC mode: mNorm−sNorm)
    // non-zero = multi-slicer (raw: mAmp−sAmp*m_spaceGain, Direwolf A+ method)
    float   m_spaceGain {0.0f};

    // DPLL state
    int32_t m_pll         {0};
    int32_t m_prevPll     {0};
    int32_t m_pllStep     {0};
    bool    m_prevDemod   {false};
    bool    m_dataDetect  {false};

    // Simple DCD sliding-window history (g+b==8 always, so g-b>=2 ⟺ g>=5)
    uint32_t m_goodHist {0};
    uint32_t m_dcdScore {0};

    // Output latch — set by nudgePll, consumed by try_demodulate
    bool    m_bitReady  {false};
    uint8_t m_readyBit  {0};
    float   m_readyConf {0.0f};

    // 256-entry cosine lookup (shared across all instances)
    static float             s_cosTable[256];
    static std::once_flag    s_cosOnce;
    static void              buildCosTable() noexcept;

    static inline float fcos(uint32_t phase) noexcept
        { return s_cosTable[(phase >> 24) & 0xffu]; }
    static inline float fsin(uint32_t phase) noexcept
        { return s_cosTable[((phase >> 24) - 64u) & 0xffu]; }

    void buildPrefilter(double fMark, double fSpace, int bitrate, int sampleRate);
    void buildRrcLowpass(int bitrate, int sampleRate);

    static float  convolve  (const float* buf, int wrPos, const float* coeffs, int taps) noexcept;
    static void   pushSample(float val, float* buf, int& wrPos, int taps) noexcept;
    void          pushLP    (float mI, float mQ, float sI, float sQ) noexcept;
    static float  agcStep   (float in, float fast, float slow,
                              float& peak, float& valley) noexcept;

    void nudgePll(float demodOut, float amplitude) noexcept;
};

} // namespace AetherDemod
