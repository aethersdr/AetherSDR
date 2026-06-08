// AetherFMDiscrimDemod.h
//
// AFSK FM-discriminator demodulator — AetherSDR / Profile B
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Algorithm: Direwolf profile-B (formerly D, renamed in v1.7)
//   BPF → center-freq IQ mix → RRC LPF → atan2 → d/dt → normalize → DPLL
//
// Complements AetherAFSKDemod (profile A).  A uses separate mark/space
// correlators; B uses a single FM discriminator at the center frequency.
// They fail on different signal types so running both in parallel (AD+)
// captures the widest range of real-world VHF FM packets.

#pragma once

#include "AetherAFSKDemod.h"   // for demod_result

#include <cstdint>
#include <vector>

namespace AetherDemod {

class AetherFMDiscrimDemod {
public:
    AetherFMDiscrimDemod() = default;

    // sliceOffset: additive offset on the normalized discriminator output.
    //   0.0f = single-slicer (B): decision at zero-crossing of norm_rate
    //   non-zero = multi-slicer (B+): evenly spread -0.5 → +0.5 across bank
    AetherFMDiscrimDemod(double fMark, double fSpace, int bitrate, int sampleRate,
                         float sliceOffset = 0.0f);

    bool try_demodulate(double sample, demod_result& result) noexcept;
    bool try_demodulate(double sample, uint8_t& bit)         noexcept;

    void reset() noexcept;

private:
    // Bandpass prefilter (wider than profile A: 0.19 × baud each side)
    std::vector<float> preCoeffs_;
    std::vector<float> preBuf_;
    int preTaps_{0};

    // Center-frequency free-running oscillator
    uint32_t cOscPhase_{0};
    uint32_t cOscDelta_{0};

    // RRC lowpass — one coefficient set, two delay lines (I and Q)
    std::vector<float> lpCoeffs_;
    std::vector<float> cIBuf_, cQBuf_;
    int lpTaps_{0};

    // FM discriminator state
    float prevPhase_{0.0f};
    float normalizeRpsam_{0.0f};  // scales radians/sample → ±1 for mark/space

    // Additive slice offset (B+ mode)
    float sliceOffset_{0.0f};

    // DPLL state (identical to profile A)
    int32_t pll_{0};
    int32_t prevPll_{0};
    int32_t pllStep_{0};
    bool    prevDemod_{false};
    bool    dataDetect_{false};

    // DCD sliding-window history
    uint32_t goodHist_{0};
    uint32_t badHist_{0};
    uint32_t dcdScore_{0};

    // Output latch
    bool    bitReady_{false};
    uint8_t readyBit_{0};
    float   readyConf_{0.0f};

    // 256-entry cosine table (shared across all instances of this class)
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

    void nudgePll(float demodOut) noexcept;
};

} // namespace AetherDemod
