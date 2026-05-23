#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace MasterSDR {

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

    // Key state — called from any thread when RadioModel emits
    // cwKeyDownChanged.  The audio thread polls m_keyDown each block
    // and transitions state machine accordingly.
    void setKeyDown(bool down) noexcept;

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

    int                m_sampleRateHz;
    std::atomic<bool>  m_enabled{false};
    std::atomic<bool>  m_keyDown{false};
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

    // Mirror sink for the TX decode path (#2417).  Holds null until
    // AudioEngine plugs in its TX-decoder feeder.  When non-null, every
    // process() block fills a small mono buffer and hands it off — even
    // through silent stretches — so the downstream decoder sees a
    // continuous timeline of envelope-shaped key down/up samples.
    SampleTap m_sampleTap;
};

} // namespace MasterSDR
