#pragma once

#include <atomic>
#include <cstdint>

namespace MasterSDR {

// Client-side TX dynamics processor — the foundation of the Pro-XL-style
// compression chain (#1661).  Phase 1 scope: core feed-forward compressor
// with soft-knee static curve + attack/release envelope follower, plus a
// brickwall peak limiter on the output.  Later phases will add expander/
// gate, de-esser, tube, enhancer, low contour, and IKA/IRC auto modes.
//
// Thread model mirrors ClientEq: the UI thread writes parameters via
// set*()  setters that update atomics and bump a version counter; the
// audio thread reads the version once per block and recaches the values.
// No locks, no allocations in process(), no exceptions.
//
// Stereo-linked detection: the envelope is driven by max(|L|, |R|) and
// the computed gain multiplier is applied identically to both channels
// so phase coherence is preserved.
class ClientComp {
public:
    ClientComp();
    ~ClientComp() = default;

    ClientComp(const ClientComp&)            = delete;
    ClientComp& operator=(const ClientComp&) = delete;

    // Main thread — call before first process() and on sample-rate change.
    void prepare(double sampleRate);

    // Main thread — global enable / bypass. Lock-free.
    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Core compressor parameters.
    void  setThresholdDb(float db) noexcept;
    float thresholdDb() const noexcept;
    void  setRatio(float ratio) noexcept;        // 1.0 = bypass, 20.0 = limiter
    float ratio() const noexcept;
    void  setAttackMs(float ms) noexcept;
    float attackMs() const noexcept;
    void  setReleaseMs(float ms) noexcept;
    float releaseMs() const noexcept;
    void  setKneeDb(float db) noexcept;
    float kneeDb() const noexcept;
    void  setMakeupDb(float db) noexcept;
    float makeupDb() const noexcept;

    // Brickwall limiter on the output.  Ceiling in dBFS (negative).
    void  setLimiterEnabled(bool on) noexcept;
    bool  limiterEnabled() const noexcept;
    void  setLimiterCeilingDb(float db) noexcept;
    float limiterCeilingDb() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush envelope state (e.g. on TX start).
    void reset() noexcept;

    // UI thread — read-only snapshots of the latest detector state for
    // meters. Updated once per block on the audio thread via atomics.
    float inputPeakDb() const noexcept;       // pre-compression peak
    float outputPeakDb() const noexcept;      // post-limiter peak
    float gainReductionDb() const noexcept;   // comp GR in dB (≤ 0)
    float limiterGrDb() const noexcept;       // limiter-only GR in dB (≤ 0)
    bool  limiterActive() const noexcept;     // true while limiter is clamping

    // Sample rate this comp was prepared at.
    double sampleRate() const noexcept { return m_sampleRate; }

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<float>    thresholdDb{-18.0f};
        std::atomic<float>    ratio{3.0f};
        std::atomic<float>    attackMs{20.0f};
        std::atomic<float>    releaseMs{200.0f};
        std::atomic<float>    kneeDb{6.0f};
        std::atomic<float>    makeupDb{0.0f};
        std::atomic<bool>     limEnabled{true};
        std::atomic<float>    limCeilingDb{-1.0f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        float thresholdDb{-18.0f};
        float ratioInv{1.0f / 3.0f};       // 1/ratio, pre-computed
        float attackCoeff{0.0f};           // 1 - exp(-1 / (fs · τ))
        float releaseCoeff{0.0f};
        float kneeDb{6.0f};
        float makeupLin{1.0f};
        bool  limEnabled{true};
        float limCeilingLin{0.891f};       // 10^(-1/20)
        float limAttackCoeff{0.0f};
        float limReleaseCoeff{0.0f};
    };

    // Meter snapshots — atomic stores on the audio thread after each
    // block, atomic loads on the UI thread for paint.
    struct Meters {
        std::atomic<float> inputPeakDb{-120.0f};
        std::atomic<float> outputPeakDb{-120.0f};
        std::atomic<float> gainReductionDb{0.0f};
        std::atomic<float> limiterGrDb{0.0f};
        std::atomic<bool>  limiterActive{false};
    };

    void recacheIfDirty() noexcept;
    float staticCurveGainDb(float envDb) const noexcept;

    double   m_sampleRate{24000.0};
    Atomics  m_atomics;
    Cached   m_cached;
    Meters   m_meters;

    // Audio-thread state — accessed only from process().
    uint64_t m_lastVersion{0};
    // Linear-domain peak envelope: tracks max(|L|,|R|) with attack/release
    // ballistics. Converting to dB after smoothing gives proper peak
    // tracking — log-domain smoothing of a sine would settle ~4 dB below
    // the peak (average of log|sin|), which is wrong for a peak compressor.
    float    m_envLin{0.0f};
    float    m_limEnvLin{0.0f};       // limiter envelope (linear)
};

} // namespace MasterSDR
