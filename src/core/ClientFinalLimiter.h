#pragma once

#include <atomic>
#include <cstdint>

#include <QtGlobal>

namespace MasterSDR {

// Final-stage brickwall limiter for the TX audio chain — sits at the
// very tail of the chain, after every user-configurable stage (Gate,
// EQ, Comp, DeEss, Tube, PUDU, Reverb) AND after the PC mic gain
// scaling.  Its job is to ensure no sample escapes louder than the
// configured ceiling, regardless of what the upstream chain does (a
// reverb tail spike, an over-driven PUDU, or a mic-gain user error).
//
// Topology: feed-forward peak limiter with a per-block smoothed
// envelope (fast attack, moderately fast release) applied as a single
// channel-linked gain, so stereo imaging is preserved.  The limiter
// reads `enabled` and `ceilingDb` lock-free per block; the audio
// thread publishes `gainReductionDb` and `active` for the UI.
//
// Thread model mirrors ClientComp / ClientReverb: UI thread writes
// atomics + bumps a version counter, audio thread reads the version
// once per block and recaches derived values.  No locks, no
// allocations, no exceptions.
class ClientFinalLimiter {
public:
    ClientFinalLimiter();
    ~ClientFinalLimiter() = default;

    ClientFinalLimiter(const ClientFinalLimiter&)            = delete;
    ClientFinalLimiter& operator=(const ClientFinalLimiter&) = delete;

    void prepare(double sampleRate);

    void  setEnabled(bool on) noexcept;
    bool  isEnabled() const noexcept;

    // Ceiling in dBFS (negative).  Clamped to [-12, 0] dB — a final-
    // stage limiter sitting before VITA-49 packetization shouldn't
    // need more than 12 dB of headroom shaving.
    void  setCeilingDb(float db) noexcept;
    float ceilingDb() const noexcept;

    // Master output trim applied AFTER the limiter.  Useful for
    // setting average level independently of the brickwall ceiling
    // (ceiling caps peaks; trim sets RMS).  Range [-12, +12] dB.
    void  setOutputTrimDb(float db) noexcept;
    float outputTrimDb() const noexcept;

    // 25 Hz one-pole HPF applied per channel BEFORE the limiter to
    // strip any DC offset the upstream chain may have introduced.
    // Cheap insurance against modulators that don't like DC.
    void  setDcBlockEnabled(bool on) noexcept;
    bool  dcBlockEnabled() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush envelope state (e.g. on TX start).
    void reset() noexcept;

    // UI-thread meter snapshots.
    float inputPeakDb()      const noexcept;  // pre-limiter peak
    float outputPeakDb()     const noexcept;  // post-limiter peak (the
                                              // value the radio actually
                                              // sees)
    float outputRmsDb()      const noexcept;  // ~300 ms post-limiter RMS
    float gainReductionDb()  const noexcept;  // ≤ 0 dB
    bool  active()           const noexcept;  // true while limiter is
                                              // clamping this block
    // Counter of pre-limiter samples that touched 0 dBFS (useful for
    // a latched OVR indicator).  Monotonically increasing; UI samples
    // the delta to detect new clipping events.
    quint64 clipPreLimiterCount() const noexcept;
    // Fraction of samples in the trailing ~3 s where the limiter was
    // actively clamping.  Range [0, 1].
    float   limiterActivityPct() const noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    struct Atomics {
        std::atomic<bool>     enabled{true};
        std::atomic<float>    ceilingDb{-1.0f};
        std::atomic<float>    outputTrimDb{0.0f};
        std::atomic<bool>     dcBlock{true};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        bool  enabled{true};
        float ceilingLin{0.891f};       // 10^(-1/20)
        float trimLin{1.0f};            // 10^(0/20)
        bool  dcBlock{true};
        float attackCoeff{0.0f};
        float releaseCoeff{0.0f};
        float rmsCoeff{0.0f};           // 300 ms one-pole RMS smoother
        float dcCoeff{0.0f};            // 25 Hz DC-block HPF coefficient
    };

    struct Meters {
        std::atomic<float>   inputPeakDb{-120.0f};
        std::atomic<float>   outputPeakDb{-120.0f};
        std::atomic<float>   outputRmsDb{-120.0f};
        std::atomic<float>   gainReductionDb{0.0f};
        std::atomic<bool>    active{false};
        std::atomic<quint64> clipPreLimiterCount{0};
        std::atomic<float>   limiterActivityPct{0.0f};
    };

    void recacheIfDirty() noexcept;

    Atomics m_atomics;
    Cached  m_cached;
    Meters  m_meters;
    uint64_t m_cachedVersion{static_cast<uint64_t>(-1)};
    double   m_sampleRate{24000.0};

    // Single-channel-linked gain envelope.  Starts at 1.0 (no
    // reduction); rises with peak overage; decays back to 1.0 at the
    // release rate.
    float m_envLin{1.0f};

    // Mean-square accumulator for the post-limiter RMS readout.
    // One-pole smoother: m_msAcc += rmsCoeff * (in² − m_msAcc).
    float m_msAcc{0.0f};

    // Per-channel DC-block HPF state.  y[n] = x[n] − x_prev + dcCoeff·y_prev.
    // dcCoeff close to 1.0 places the corner at ~25 Hz.
    float m_dcXprevL{0.0f};
    float m_dcYprevL{0.0f};
    float m_dcXprevR{0.0f};
    float m_dcYprevR{0.0f};

    // Trailing-window limiter activity ratio: count of samples that
    // tripped the limiter divided by total samples in the window.
    // Implemented as an exponential one-pole over per-block ratios.
    float m_activityAcc{0.0f};
};

} // namespace MasterSDR
