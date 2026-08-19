#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace AetherSDR {

// Client-side CW sidetone generator.  Produces a clean sine tone in the
// AudioEngine's RX output stream while the operator's CW key is held,
// driven by RadioModel::sendCwKey() / sendCwPaddle() events.
//
// Why client-side?  The radio's own sidetone (audio_mute=1 + DAX feed)
// has 30–100 ms of round-trip latency — fine for slow CW, brutal at
// 25+ WPM.  This generator runs in the audio thread alongside the
// existing buffer mix in AudioEngine, so the operator hears the tone
// within one audio callback (~10 ms) of pressing the key.
//
// All parameter setters use std::atomic so the UI thread can update
// pitch / volume / enable live without locking the audio thread.
// process() is the only method called on the audio thread.
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

    // Key state — called from any thread, and concurrently from several
    // in practice (iambic worker, CWX worker, GUI-thread handlers of
    // RadioModel::cwKeyDownChanged); producers serialize on a short
    // spinlock the audio thread never touches.  Each call is timestamped
    // inside the lock and queued; process() applies the transition at
    // the exact sample offset the timestamp maps to, so key edges are no
    // longer quantized to audio block boundaries (#4809 — up to one
    // whole block of jitter per edge, and far more on the push-model
    // QAudioSink sink).  On queue overflow the last-known state still
    // lands at the next block start (the pre-#4809 behavior) via
    // m_keyDown.
    void setKeyDown(bool down) noexcept;

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
    // Size 64 is ~16 elements of headroom: 2 slots per element (down + up),
    // doubled again because every element arrives twice (worker-direct plus
    // the GUI cwKeyDownChanged echo).  Still far beyond what fits in one
    // audio block even at 60 WPM.
    struct KeyEdge {
        std::chrono::steady_clock::time_point t;
        bool down;
    };
    static constexpr uint32_t kEdgeQueueSize = 64;  // power of two

    // Release the timestamp→sample anchor only after this much continuous
    // idle: longer than any inter-element or inter-character gap at
    // typical paddle speeds (180 ms inter-character at 20 WPM), short
    // enough to bound steady_clock vs audio-clock drift between keying
    // sequences.  Below 15 WPM an inter-character gap (3 units of
    // 1200/wpm ms) exceeds this and the anchor is released between
    // characters — harmless, since at those speeds a unit is >=85 ms and
    // block quantization of the next onset is inaudible; element lengths
    // stay exact either way, because they come from the keyer's grid,
    // not the anchor.
    //
    // process() reuses this same threshold for the two guards that keep the
    // mapping honest when it is NOT pumped in real time: how far the anchor
    // may drift from wall clock before it is dropped, and how old a queued
    // edge may be before a fresh anchor discards it instead of replaying it.
    static constexpr int kReanchorIdleMs = 250;

    // Upper bound on m_anchorSlack (#4890) — the headroom a NEW anchor starts
    // with, and the only offset that is carried rather than corrective.  It
    // sits inside the staleness guard's margin (a fresh anchor is placed at
    // blockStart + slack, so the mapping starts that far ahead of the head),
    // hence the relationship the assert pins.
    //
    // The per-edge forward shift is deliberately NOT bounded by this or any
    // constant.  A shift is a re-alignment, not drift: it moves the mapping to
    // the render head, so afterwards the guard quantity is the wall time since
    // that edge, not a running total.  Measured over a sustained racing burst
    // (119 shifts) the quantity peaked at 2.4 ms against this 250 ms threshold
    // and never re-anchored.  Capping the shift was tried and is actively
    // harmful: it strands the mapping behind the head, so every later edge in
    // that anchor clamps and element durations collapse to block multiples
    // (measured: 5.0 ms elements rendering as 2.8 ms) — the exact defect this
    // change exists to remove.
    static constexpr int kAnchorSlackCapMs = 40;
    static_assert(kAnchorSlackCapMs < kReanchorIdleMs,
                  "carried slack must leave the staleness guard margin");
    // The assert pins the SAFETY relationship, but it is not the binding
    // constraint on this value: slack IS onset latency, so the ceiling that
    // matters is the 30-100 ms radio round trip this generator exists to beat
    // (see m_anchorSlack).  A cap at the top of that range would let the
    // client tone silently become no better than the radio's.  40 ms keeps it
    // clearly ahead while leaving room for several refills' worth of learning:
    // a 22 s hand-keyed session measured 5.5 ms of total learned slack, and
    // the deliberate 800 ms pump stall in the decay test needs only enough to
    // clear one refill.
    // The cap also SPENDS the staleness guard's stall tolerance rather than
    // merely staying under it: a fresh anchor starts at blockStart + slack,
    // so at the cap a genuine pump stall has kReanchorIdleMs - 40 ms of
    // margin left, not the full 250 ms.

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

    // Learned anchor headroom, in samples (#4890).  A push-model sink keeps
    // its device buffer full, so m_streamPos advances at wall-clock pace and
    // every edge target races the render head: an edge whose exact position
    // was already rendered used to be clamped to the current block start,
    // quantizing BOTH edges of an element to block boundaries (measured on
    // Linux: dit SD 0.2 ms at emission -> 6-8 ms rendered, in whole-block
    // steps).  When an edge arrives late, process() now shifts the whole
    // anchor forward instead — relative spacing (the rhythm) survives, only
    // absolute onset latency grows — and the deficit accumulates here so the
    // NEXT burst anchors with enough headroom to stop racing at all.
    // Bounded by kAnchorSlackCapMs; reset with the mapping.
    //
    // It decays as well as grows, and must: this generator exists because the
    // radio's own sidetone costs 30-100 ms round trip, so a slack that only
    // ratcheted upward would let one transient stall park the client tone
    // inside the very range it is meant to avoid, for the rest of the
    // session.  An anchor that completed without ever running late is
    // evidence the sink has headroom to spare, and halves it.
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
