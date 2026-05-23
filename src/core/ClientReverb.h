#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace MasterSDR {

// Client-side reverb — TX DSP chain Phase 6 (Freeverb).  Eight parallel
// lowpass-feedback comb filters in parallel summed through four series
// allpass filters, stereo-spread by 23 samples between L and R.  A
// pre-delay ring buffer sits in front of the reverb core.  Voice-
// oriented knob set; no "studio" parameters.
//
// Thread model mirrors ClientTube / ClientGate / ClientDeEss: UI
// thread writes atomics + bumps a version counter; the audio thread
// reads the version once per block and recaches derived values.  No
// locks, no allocations in process(), no exceptions.
//
// Buffer sizes are fixed in prepare() based on sample rate — max comb
// length + stereo-spread headroom for Size=1, plus max pre-delay of
// 100 ms.  Typical TX path runs at 24 kHz; total buffer budget per
// ClientReverb instance is ~12 kB of float samples.
class ClientReverb {
public:
    ClientReverb();
    ~ClientReverb() = default;

    ClientReverb(const ClientReverb&)            = delete;
    ClientReverb& operator=(const ClientReverb&) = delete;

    // Main thread — call before first process() and on sample-rate change.
    void prepare(double sampleRate);

    void  setEnabled(bool on) noexcept;
    bool  isEnabled() const noexcept;

    void  setSize(float s) noexcept;          float size() const noexcept;       // 0..1
    void  setDecayS(float s) noexcept;        float decayS() const noexcept;     // 0.3..5
    void  setDamping(float d) noexcept;       float damping() const noexcept;    // 0..1
    void  setPreDelayMs(float ms) noexcept;   float preDelayMs() const noexcept; // 0..100
    void  setMix(float m) noexcept;           float mix() const noexcept;        // 0..1

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;
    void reset() noexcept;

    // UI-thread meter snapshots.
    float inputPeakDb() const noexcept;
    float outputPeakDb() const noexcept;
    float wetRmsDb()   const noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<float>    size{0.5f};
        std::atomic<float>    decayS{1.2f};
        std::atomic<float>    damping{0.5f};
        std::atomic<float>    preDelayMs{20.0f};
        std::atomic<float>    mix{0.15f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        float combFeedback[8]{};   // per-comb feedback coefficient
        int   combLen[8]{};        // active comb length (size-scaled)
        int   allpassLen[4]{};     // active allpass length
        float damp1{0.5f};         // damping filter coefficients
        float damp2{0.5f};
        float mix{0.15f};
        int   preDelaySamples{0};
        float dryGain{0.85f};
        float wetGain{0.15f};
    };

    // A single lowpass-feedback comb filter.  Internal LPF state is a
    // one-pole filter applied to the feedback path — what makes this
    // "Freeverb" and not a plain Schroeder comb.
    struct Comb {
        std::vector<float> buffer;   // per-channel delay line
        int                index{0};
        float              lpfState{0.0f};
    };

    struct Allpass {
        std::vector<float> buffer;
        int                index{0};
    };

    void recacheIfDirty() noexcept;

    // Reference comb + allpass lengths at 44.1 kHz (Jezar's original
    // public-domain Freeverb tunings).  Scaled by sampleRate/44100 in
    // prepare(), further scaled by Size in recacheIfDirty.
    static constexpr int kNumCombs     = 8;
    static constexpr int kNumAllpasses = 4;
    static constexpr int kStereoSpread = 23;

    struct Channel {
        Comb    combs[kNumCombs];
        Allpass allpasses[kNumAllpasses];
    };

    double   m_sampleRate{24000.0};
    Atomics  m_atomics;
    Cached   m_cached;

    // Allocated once in prepare().
    Channel  m_chL;
    Channel  m_chR;

    // Pre-delay: stereo-interleaved ring buffer sized for kMaxPreDelayMs.
    static constexpr int kMaxPreDelayMs = 100;
    std::vector<float> m_preDelay;       // frames × 2
    int m_preDelayCap{0};                // capacity in frames
    int m_preDelayWrite{0};

    // Sample-rate-scaled maxima (set in prepare()).  The size-scaled
    // active lengths in Cached can't exceed these.
    int m_maxCombLen[kNumCombs]{};
    int m_maxAllpassLen[kNumAllpasses]{};

    uint64_t m_lastVersion{0};

    // Meters (UI-thread readable).
    std::atomic<float> m_inputPeakDb{-120.0f};
    std::atomic<float> m_outputPeakDb{-120.0f};
    std::atomic<float> m_wetRmsDb{-120.0f};
};

} // namespace MasterSDR
