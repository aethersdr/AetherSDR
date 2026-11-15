#pragma once

// AetherTX multiband limiter (RFC #4836): four Linkwitz-Riley bands, each
// with its own look-ahead peak limiter, summed back together. It sits after
// the chain and just ahead of Peak control, so per-band peaks (a boomy low, a
// sibilant high) are caught before the envelope clipper has to.
//
// Crossovers: LR4 (two Butterworth sections) split at X2, then the low side
// at X1 and the high side at X3. The low side also runs an X3 all-pass and the
// high side an X1 all-pass, so the four bands sum to a flat-magnitude
// all-pass.
//
// Limiter: every band is delayed by the look-ahead L. The gain each sample
// needs is released toward 1 at the band's Release, a sliding minimum over
// L + 1 samples takes the lowest, and a moving average over L + 1 smooths it.
// The gain applied to any sample is then at most what that sample needs, so
// no band sample leaves above its threshold.
//
// Parameters are atomics the audio thread applies once per block; nothing
// allocates after construction. Mono: the first channel is processed and
// copied to the rest, as the mic is.

#include <array>
#include <atomic>
#include <cstdint>

#include "core/Biquad.h"

namespace AetherSDR {

class ClientMbLimiter {
public:
    static constexpr int kBands = 4;
    static constexpr int kCrossovers = kBands - 1;
    static constexpr double kRate = 48000.0;
    static constexpr int kLookahead = 240;  // 5 ms at 48 kHz
    static constexpr float kMinThresholdDb = -24.0f;
    static constexpr float kMaxThresholdDb = 0.0f;
    static constexpr float kDefaultThresholdDb = -6.0f;
    static constexpr float kMinReleaseMs = 10.0f;
    static constexpr float kMaxReleaseMs = 1000.0f;
    static constexpr float kDefaultReleaseMs = 100.0f;
    // Neighboring crossovers stay at least this ratio apart.
    static constexpr float kMinCrossoverRatio = 1.5f;
    static constexpr std::array<float, kCrossovers> kMinCrossoverHz{80.0f, 500.0f, 2000.0f};
    static constexpr std::array<float, kCrossovers> kMaxCrossoverHz{500.0f, 2000.0f, 6000.0f};
    static constexpr std::array<float, kCrossovers> kDefaultCrossoverHz{250.0f, 1000.0f, 3000.0f};

    ClientMbLimiter();

    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Clamped to the crossover's own range, then kept kMinCrossoverRatio from
    // its neighbors.
    void setCrossoverHz(int index, float hz) noexcept;
    float crossoverHz(int index) const noexcept;
    // All three at once: X2 first, then X1 and X3 around it. Profiles use
    // this so one order of arrival cannot block another.
    void setCrossoversHz(float x1, float x2, float x3) noexcept;

    void setThresholdDb(int band, float db) noexcept;  // -24..0 dBFS
    float thresholdDb(int band) const noexcept;
    void setReleaseMs(int band, float ms) noexcept;    // 10..1000 ms
    float releaseMs(int band) const noexcept;

    void process(float* interleaved, int frames, int channels) noexcept;
    void reset() noexcept;
    int latencyFrames() const noexcept { return kLookahead; }

    // The deepest gain reduction on `band` since the last call, in dB (<= 0).
    float takeGainReductionDb(int band) noexcept;

    static constexpr int kWindow = kLookahead + 1;

    // One band's look-ahead peak limiter: process(x) returns the sample
    // kLookahead calls ago, never above `threshold` in magnitude. Public so
    // the guarantee can be tested on its own; the sum of four bands is not
    // bounded by any one band's threshold.
    struct BandLimiter {
        void configure(float thresholdLinear, float releaseMs, double sampleRate) noexcept;
        std::array<float, kLookahead> delay{};
        int delayPos{0};
        float held{1.0f};  // the released required gain
        // Sliding minimum over the last kWindow held gains: a monotonic
        // queue of (gain, sample index) in a fixed ring.
        std::array<float, kWindow> qGain{};
        std::array<int64_t, kWindow> qIndex{};
        int qHead{0};
        int qSize{0};
        // Moving average of the minimum over kWindow samples.
        std::array<float, kWindow> avgRing{};
        int avgPos{0};
        double avgSum{kWindow};
        float threshold{0.5f};
        float releaseCoeff{0.0f};
        float minGain{1.0f};  // audio-thread side of the GR meter
        void clear() noexcept;
        float process(float x, int64_t n) noexcept;
    };

private:

    void applyParameters() noexcept;
    void updateFilters() noexcept;
    bool glideCrossovers() noexcept;  // true while still moving
    void processFrames(float* interleaved, int frames, int channels) noexcept;
    void publishGainReduction() noexcept;
    void clearState() noexcept;
    static float clampCrossover(int index, float hz, float below, float above) noexcept;

    double m_sampleRate{kRate};
    std::atomic<bool> m_enabled{false};
    std::array<std::atomic<float>, kCrossovers> m_crossoverHz{};
    // The crossovers the filters run at: they glide to the set ones by at
    // most kGlideStep per kGlideFrames, so a change does not click.
    static constexpr int kGlideFrames = 32;
    static constexpr double kGlideStep = 1.02;
    std::array<double, kCrossovers> m_crossoverTarget{};
    std::array<double, kCrossovers> m_crossoverLive{};
    std::array<std::atomic<float>, kBands> m_thresholdDb{};
    std::array<std::atomic<float>, kBands> m_releaseMs{};
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_flushRequested{true};
    uint64_t m_appliedVersion{0};

    // Two sections make one LR4 filter.
    std::array<Biquad, 2> m_lp2{}, m_hp2{}, m_lp1{}, m_hp1{}, m_lp3{}, m_hp3{};
    Biquad m_ap3Low{}, m_ap1High{};
    std::array<BandLimiter, kBands> m_bands{};
    int64_t m_sampleIndex{0};
    std::array<std::atomic<float>, kBands> m_grPeak{};  // min linear gain since last take
};

}  // namespace AetherSDR
