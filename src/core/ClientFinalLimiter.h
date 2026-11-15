#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <QtGlobal>

namespace AetherSDR {

// Final nonlinear stage of the 48 kHz TX chain: after every user stage and PC mic
// gain, before the 48->24 kHz egress SRC. A look-ahead brickwall: no 48 kHz
// sample leaves above the ceiling. SRC reconstruction can still overshoot
// between samples before final Int16 saturation, and it bounds audio samples,
// not the SSB RF envelope.
//
// The signal is delayed by the look-ahead L (0..5 ms). The gain each sample
// needs (ceiling / |x|, channel-linked) is released toward 1, slid through a
// minimum over L+1 samples and then a moving average over L+1 samples, so the
// gain has ramped down to what a peak needs by the time that peak leaves the
// delay. L = 0 is an instant hard limit. A hard clip after the gain is a safety
// net only; safetyClipCount() counts the samples it touched. Under steady
// parameters it never acts; a look-ahead, ceiling or on/off change while the
// limiter is working can need it for up to one window.
//
// Changing L, or switching the limiter on or off, crossfades between the old
// and new delay taps over kCrossfadeFrames; a change that arrives during a
// fade waits for it to end. The fade covers the delay tap only: switching off
// while limiting releases the gain at once.
//
// UI thread writes atomics + bumps a version; the audio thread recaches per
// block and publishes the meters. No locks, allocations or exceptions.
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

    // Look-ahead in ms, clamped to [0, kMaxLookaheadMs]. It is also the delay
    // the limiter adds while it is enabled.
    static constexpr float kMaxLookaheadMs     = 5.0f;
    static constexpr float kDefaultLookaheadMs = 1.5f;
    static constexpr float kDefaultCeilingDb   = -0.5f;
    void  setLookaheadMs(float ms) noexcept;
    float lookaheadMs() const noexcept;

    // Delay added, in frames at the prepared rate: the look-ahead while
    // enabled, 0 while disabled. Any thread.
    int latencyFrames() const noexcept;

    // Drive trim applied before the limiter. Positive values increase limiter
    // activity; negative values lower the signal below the ceiling. Range
    // [-12, +12] dB.
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
    float outputPeakDb()     const noexcept;  // post-limiter 48 kHz peak;
                                              // egress SRC may overshoot it
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
    // Samples the safety clip after the look-ahead gain had to touch.
    // Monotonic. Stays 0 under steady parameters; a look-ahead, ceiling or
    // on/off change while the limiter is working can need it.
    quint64 safetyClipCount() const noexcept;

    double sampleRate() const noexcept
    {
        return m_sampleRate.load(std::memory_order_relaxed);
    }

    // Capacity: 5 ms at 48 kHz, the TX DSP rate. A higher prepared rate
    // caps the look-ahead at this many frames.
    static constexpr int kMaxLookaheadFrames = 240;
    static constexpr int kCrossfadeFrames    = 240;

private:
    struct Atomics {
        std::atomic<bool>     enabled{true};
        std::atomic<float>    ceilingDb{kDefaultCeilingDb};
        std::atomic<float>    lookaheadMs{kDefaultLookaheadMs};
        std::atomic<float>    outputTrimDb{0.0f};
        std::atomic<bool>     dcBlock{true};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        bool  enabled{true};
        float ceilingLin{0.944f};       // 10^(-0.5/20)
        int   lookaheadFrames{72};      // 1.5 ms at 48 kHz
        float trimLin{1.0f};            // 10^(0/20)
        bool  dcBlock{true};
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
        std::atomic<quint64> safetyClipCount{0};
    };

    void recacheIfDirty() noexcept;
    int  framesForMs(float ms, double rate) const noexcept;
    // Restart the gain path for a window of `window` samples, every slot
    // holding `gain`.
    void resetGainPath(int window, float gain) noexcept;

    Atomics m_atomics;
    Cached  m_cached;
    Meters  m_meters;
    uint64_t m_cachedVersion{static_cast<uint64_t>(-1)};
    std::atomic<double> m_sampleRate{24000.0};

    // Delay ring, one slot per frame (L and R), indexed by m_sampleIndex.
    static constexpr int kRing = kMaxLookaheadFrames + 1;
    std::array<float, kRing> m_delayL{};
    std::array<float, kRing> m_delayR{};
    int64_t m_sampleIndex{0};

    // The delay in use (L, or 0 while disabled), and the one being faded
    // out of after a change.
    int m_tap{-1};
    int m_fadeFromTap{0};
    int m_fadeLeft{0};

    // Gain path: the released needed gain, its sliding minimum (a
    // monotonic ring of (gain, index)) and the moving average of that
    // minimum, all over m_window = L + 1 samples.
    float m_held{1.0f};
    int   m_window{1};
    std::array<float, kRing>   m_qGain{};
    std::array<int64_t, kRing> m_qIndex{};
    int   m_qHead{0};
    int   m_qSize{0};
    std::array<float, kRing> m_avgRing{};
    int    m_avgPos{0};
    double m_avgSum{1.0};

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

} // namespace AetherSDR
