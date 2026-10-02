#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace AetherSDR {

// Client-side CW sidetone: a sine in AudioEngine's RX output while the key is
// held. The radio's sidetone path has 30-100 ms round trip; this runs on the
// audio thread, within one callback (~10 ms) of the key. Setters are atomics
// (pitch/volume/enable live from the UI); process() is the only audio-thread
// method.
class CwSidetoneGenerator {
public:
    explicit CwSidetoneGenerator(int sampleRateHz = 48000);

    // UI-thread setters — all atomic, safe to call any time.
    void setEnabled(bool on) noexcept;
    void setPitchHz(float hz) noexcept;       // clamped to [100, 4000]
    void setVolume(float v) noexcept;         // 0.0..1.0
    void setShapingMs(float ms) noexcept;     // raised-cosine ramp, [0, 50]
    void setPan(float p) noexcept;            // 0.0=L, 0.5=center, 1.0=R

    // Sample-rate change — call only when audio output is paused / not
    // racing process().  Resets phase + ramp state to avoid mid-cycle
    // glitches.  Used when the dedicated sidetone sink negotiates a
    // different rate than the requested 48 kHz.
    void setSampleRateHz(int hz) noexcept;
    int  sampleRateHz() const noexcept { return m_sampleRateHz; }

    // Key state, callable concurrently from any thread (iambic worker, CWX worker,
    // GUI-thread straight key); producers serialize on a spinlock the audio thread
    // never touches. Each edge is timestamped and queued, and process() applies it
    // at the exact sample offset its stamp maps to (#4809). On queue overflow the
    // last state lands at the next block start via m_keyDown. `when` (#4890): the
    // scheduled instant for producers with an exact grid, else now(). Stamps are
    // clamped monotonic inside the lock because process() needs queue order ==
    // time order.
    void setKeyDown(bool down,
                    std::chrono::steady_clock::time_point when =
                        std::chrono::steady_clock::now()) noexcept;

    // Late-edge branch fire count since reset() (#4890).  Exists so a test can
    // prove it exercised that branch rather than passing vacuously; read from
    // the same thread as process().
    int64_t shiftCount() const noexcept { return m_shiftCount; }

    // Staleness re-anchors since reset() (#4890) — mapping dropped because
    // wall clock ran away from the render head.  Read from process()'s thread.
    int64_t staleReanchorCount() const noexcept { return m_staleReanchors; }

    bool  isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }
    float pitchHz() const noexcept   { return m_pitchHz.load(std::memory_order_relaxed); }
    float volume() const noexcept    { return m_volume.load(std::memory_order_relaxed); }
    float pan() const noexcept       { return m_pan.load(std::memory_order_relaxed); }

    // Audio-thread: add sidetone samples to interleaved stereo float32
    // output (frames * 2 floats).  Mixes additively, so existing audio
    // in `out` is preserved.  Returns true if any non-zero samples were
    // mixed (useful for skipping clamp work in callers).
    bool process(float* out, int frames) noexcept;

    // Reset state machine to idle (e.g., on disconnect).  Audio-thread.
    void reset() noexcept;

    // Optional sample mirror — when set, every process() block also
    // delivers the pre-pan mono sidetone signal (same envelope and
    // pitch the operator hears) to this callback.  Used by the TX
    // decode path (#2417) to feed ggmorse with the operator's own
    // keying timing without tapping the RF/audio path on the radio.
    // The callback runs on the audio thread; the implementation must
    // be lock-free and short.  Set once at construction time before
    // process() is first called from the sink — not safe to swap
    // while audio is running.
    using SampleTap =
        std::function<void(const float* monoSamples, int frames, int sampleRateHz)>;
    void setSampleTap(SampleTap tap) noexcept { m_sampleTap = std::move(tap); }

private:
    enum class State : uint8_t { Idle, RampUp, Sustain, RampDown };

    // One timestamped key transition from setKeyDown().  The queue is a
    // fixed-size MPSC ring: producers = the keying threads (head,
    // serialized by m_edgeLock), consumer = the audio thread (tail).
    // Size 64 is ~32 elements of headroom at 2 slots per element (down +
    // up).  It was sized for every element arriving twice (worker-direct
    // plus a GUI cwKeyDownChanged echo); #4976 removed that echo for keyer
    // edges, so the doubling is now pure headroom — still far beyond what
    // fits in one audio block even at 60 WPM.
    struct KeyEdge {
        std::chrono::steady_clock::time_point t;
        bool down;
    };
    static constexpr uint32_t kEdgeQueueSize = 64;  // power of two

    // Newest stamp ever queued — the monotonic floor for caller-supplied
    // scheduled instants (see setKeyDown).  Guarded by m_edgeLock.
    std::chrono::steady_clock::time_point m_lastQueuedStamp{};

    // Release the timestamp->sample anchor after this much continuous idle: longer
    // than element/character gaps at typical speeds (180 ms at 20 WPM), short enough
    // to bound steady_clock vs audio-clock drift. Below 15 WPM the anchor releases
    // between characters (harmless; element lengths come from the keyer grid).
    // process() reuses it for the staleness and stale-edge guards.
    static constexpr int kReanchorIdleMs = 250;

    // Upper bound on m_anchorSlack (#4890): the headroom a NEW anchor starts with. A
    // fresh anchor sits at blockStart + slack, hence the assert. The per-edge forward
    // shift is deliberately unbounded (a re-alignment, not drift; capping it
    // collapses elements to block multiples).
    static constexpr int kAnchorSlackCapMs = 40;
    static_assert(kAnchorSlackCapMs < kReanchorIdleMs,
                  "carried slack must leave the staleness guard margin");
    // The binding constraint is latency, not the assert: slack IS onset latency, and
    // this generator exists to beat the radio's 30-100 ms round trip. 40 ms stays
    // clearly ahead (a 22 s hand-keyed session learned 5.5 ms). At the cap a pump
    // stall has kReanchorIdleMs - 40 ms of staleness margin left.

    void applyKeyEdge(bool down) noexcept;  // state-machine transition

    int                m_sampleRateHz;
    std::atomic<bool>  m_enabled{false};
    std::atomic<bool>  m_keyDown{false};

    KeyEdge                m_edgeQueue[kEdgeQueueSize];
    std::atomic<uint32_t>  m_edgeHead{0};
    std::atomic<uint32_t>  m_edgeTail{0};
    std::atomic_flag       m_edgeLock;   // producer-only; audio thread never takes it
    std::atomic<float> m_pitchHz{600.0f};
    std::atomic<float> m_volume{0.5f};
    std::atomic<float> m_pan{0.5f};
    std::atomic<float> m_shapingMs{5.0f};

    // Audio-thread state — only touched in process()/reset().
    State    m_state{State::Idle};
    int      m_rampSample{0};       // current sample within the active ramp
    int      m_rampLength{240};     // recomputed from m_shapingMs
    double   m_phase{0.0};          // sine phase accumulator
    float    m_lastPitchHz{600.0f}; // for change detection (smooth transitions)
    bool     m_gateDown{false};     // audio-thread view of the key state

    // Timestamp→sample mapping for the current keying sequence.  Anchored
    // on the first edge after an idle period (that edge plays at the
    // block's start, preserving the pre-#4809 onset latency); every later
    // edge lands at anchor + Δt·rate, so relative spacing — the thing the
    // ear hears as rhythm — is sample-exact.  The anchor survives
    // inter-element and inter-character gaps (dropping it per-gap would
    // re-quantize element onsets to block boundaries) and is released
    // only after kReanchorIdleMs of continuous idle, which bounds
    // steady_clock vs audio-clock drift between keying sequences.
    int64_t  m_streamPos{0};        // samples rendered since reset()
    bool     m_haveAnchor{false};
    std::chrono::steady_clock::time_point m_anchorTime;
    int64_t  m_anchorPos{0};
    int64_t  m_idleSamples{0};      // contiguous idle samples since the last edge

    // Learned anchor headroom in samples (#4890). A push-model sink keeps its buffer
    // full, so edge targets race the render head; a late edge shifts the whole anchor
    // forward and the deficit accumulates here so the next burst anchors ahead.
    // Capped at kAnchorSlackCapMs, reset with the mapping, and halved when an anchor
    // completes without running late, so one stall can't park latency in the radio's
    // 30-100 ms range.
    int64_t  m_anchorSlack{0};

    // Whether the current anchor has ever had to shift for a late edge.  An
    // anchor released without one is evidence the sink had headroom to spare
    // for that whole burst, which is when slack gives ground.
    bool     m_anchorWentLate{false};

    // Test-only observability (#4890).  m_shiftCount: a harness that cannot
    // get the render head ahead of wall clock never exercises the late-edge
    // branch, and its assertions would then pass against the pre-fix code too,
    // so a test can require the branch actually fired.  m_staleReanchors: pins
    // that sustained racing does NOT walk the mapping into the staleness guard
    // — the concern that motivated bounding the shift, which measurement
    // refuted.
    int64_t  m_shiftCount{0};
    int64_t  m_staleReanchors{0};

    // Mirror sink for the TX decode path (#2417).  Holds null until
    // AudioEngine plugs in its TX-decoder feeder.  When non-null, every
    // process() block fills a small mono buffer and hands it off — even
    // through silent stretches — so the downstream decoder sees a
    // continuous timeline of envelope-shaped key down/up samples.
    SampleTap m_sampleTap;
};

} // namespace AetherSDR
