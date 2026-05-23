#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace MasterSDR {

// Apollo-era Quindar tones for MOX/PTT engage and disengage events
// (#2262).  When enabled and the active TX slice is on a phone mode,
// this stage:
//
//   - inserts a short tone (or Morse "K") at the start of every
//     transmission ("Engaging" phase),
//   - inserts a short tone (or Morse "BK") at the end of every
//     transmission ("Disengaging" phase) and defers the actual
//     `xmit 0` command until the outro finishes.
//
// Two stylistic flavours selected at runtime:
//
//   Tone:  2525 Hz / 2475 Hz sine, 250 ms each, 5 ms cos² envelope.
//          The classic NASA Mission Control sound.
//   Morse: pre-rendered "K" (intro) and "BK" (outro) at 45 WPM with a
//          configurable carrier pitch.  Ham-radio convention — K =
//          "go ahead", BK = "back to you".
//
// Inserted in the TX path AFTER the user's DSP chain and PC mic gain
// but BEFORE the final brickwall limiter, so the tone is unprocessed
// by Comp/EQ but still bounded by the configured ceiling.  During
// Engaging/Disengaging the stage REPLACES interleaved samples with
// the generated audio (it does not sum with mic input).
//
// Threading mirrors ClientFinalLimiter / ClientTxTestTone: UI thread
// writes std::atomic parameters and bumps a version counter; audio
// thread reads the version once per block and recaches.  No locks,
// no allocations, no exceptions in the audio path.  Phase transitions
// (Idle → Engaging → Live → Disengaging → Idle) are atomic so the
// coordinator on the GUI thread can drive them without races.
class ClientQuindarTone {
public:
    enum class Style : uint8_t { Tone = 0, Morse = 1 };
    enum class Phase : uint8_t {
        Idle        = 0,
        Engaging    = 1,
        Live        = 2,
        Disengaging = 3,
    };

    ClientQuindarTone();
    ~ClientQuindarTone() = default;

    ClientQuindarTone(const ClientQuindarTone&)            = delete;
    ClientQuindarTone& operator=(const ClientQuindarTone&) = delete;

    void prepare(double sampleRate);

    // Master enable.  When off, process() is a no-op regardless of
    // phase — the coordinator should still be able to drive phase
    // transitions safely (they're inert if the stage is disabled).
    void  setEnabled(bool on) noexcept;
    bool  isEnabled() const noexcept;

    void  setStyle(Style s) noexcept;
    Style style() const noexcept;

    // Output level in dBFS, applied to the generated waveform.
    // Range [-20, 0] dB.
    void  setLevelDb(float db) noexcept;
    float levelDb() const noexcept;

    // Tone-style frequencies (default 2525 / 2475 Hz Apollo Quindar).
    void  setIntroFreqHz(float hz) noexcept;
    float introFreqHz() const noexcept;
    void  setOutroFreqHz(float hz) noexcept;
    float outroFreqHz() const noexcept;
    void  setDurationMs(int ms) noexcept;       // 100..500
    int   durationMs() const noexcept;

    // Morse-style parameters.
    void  setMorseWpm(int wpm) noexcept;        // 20..60
    int   morseWpm() const noexcept;
    void  setMorsePitchHz(float hz) noexcept;   // 400..1200
    float morsePitchHz() const noexcept;

    // Compute outro duration in ms for the current style + settings.
    // Used by the PTT coordinator to size the deferred-xmit-0 timer.
    // For Tone style: durationMs(); for Morse style: BK length at the
    // current WPM (~560 ms at 45 WPM).
    int   currentOutroDurationMs() const noexcept;
    int   currentIntroDurationMs() const noexcept;

    // Phase control — main thread.  Atomic store; audio thread picks
    // it up on the next block.
    void  startIntro() noexcept;     // → Engaging
    void  startOutro() noexcept;     // → Disengaging
    void  forceIdle() noexcept;      // → Idle (e.g. on disconnect)

    // Coalesce a re-engage during the outro window.  If the current
    // phase is Disengaging, force back to Live (skipping a fresh
    // intro so the user doesn't feel an outro+intro dead zone).
    // No-op otherwise.  Returns true if a coalesce actually happened.
    bool  coalesceReEngage() noexcept;

    Phase phase() const noexcept;

    // Invoked on the GUI thread (via the supplied dispatcher) when a
    // phase finishes.  The audio thread sets a "phase done" atomic
    // flag; the coordinator polls or queues a signal off it.  In
    // practice TransmitModel uses a QTimer::singleShot for the outro
    // that's sized from currentOutroDurationMs() rather than relying
    // on a callback hop, so this hook is mainly for tests + parity
    // with the spec.
    using PhaseCompleteCallback = std::function<void(Phase finished)>;
    void  setPhaseCompleteCallback(PhaseCompleteCallback cb);

    // Audio-thread entry points.  channels must be 1 or 2.  When
    // phase is Idle or Live, samples pass through unchanged.  When
    // Engaging or Disengaging, samples are overwritten with the
    // generated tone/Morse audio.  Phase transitions Engaging → Live
    // and Disengaging → Idle happen automatically when the configured
    // duration is consumed.
    void  process(int16_t* interleaved, int frames, int channels) noexcept;
    void  process(float*   interleaved, int frames, int channels) noexcept;

    // Local sidetone fill.  Called from the dedicated QuindarLocalSink
    // each audio block so the operator hears the tone the moment MOX
    // is hit — Quindar must always be locally audible whenever it's
    // overlaying the TX stream.  Writes to the stereo float32 output
    // buffer using independent local-rate phase state (separate from
    // process(), which runs at the radio's 24 kHz TX rate).  When the
    // atomic phase is Idle or Live, leaves the buffer as zeros.  Never
    // mutates the atomic phase — the TX-path process() is the source
    // of truth for transitions; this path just mirrors them.
    void  processSidetone(float* stereoOut, int frames,
                          double sidetoneSampleRate) noexcept;

    void  reset() noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<uint8_t>  style{static_cast<uint8_t>(Style::Tone)};
        std::atomic<float>    levelDb{-6.0f};
        std::atomic<float>    introFreqHz{2525.0f};
        std::atomic<float>    outroFreqHz{2475.0f};
        std::atomic<int>      durationMs{250};
        std::atomic<int>      morseWpm{45};
        std::atomic<float>    morsePitchHz{750.0f};
        std::atomic<uint8_t>  phase{static_cast<uint8_t>(Phase::Idle)};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        bool  enabled{false};
        Style style{Style::Tone};
        float ampLin{0.5f};
        float introPhaseInc{0.0f};
        float outroPhaseInc{0.0f};
        float morsePhaseInc{0.0f};
        int   toneFrames{0};         // duration of one tone in frames
        int   introMorseFrames{0};   // length of "K" element timeline
        int   outroMorseFrames{0};   // length of "BK" element timeline
        int   morseDotFrames{0};     // 1200 / WPM ms → frames
    };

    // Element-table entry for the Morse renderer: a contiguous run of
    // either tone-on or silence.  Computed once per parameter change.
    struct MorseSegment {
        int  startFrame{0};
        int  lengthFrames{0};
        bool toneOn{true};
    };

    void recacheIfDirty() noexcept;
    void rebuildMorseTables() noexcept;

    // Generate the next sample of an envelope-shaped sine and advance
    // phase.  `phaseFrame` is the frame index within the active phase
    // (0-based, monotonic).  Used by both int16 and float paths.
    float generateToneSample(int phaseFrame, int totalFrames,
                             float phaseInc) noexcept;
    float generateMorseSample(int phaseFrame,
                              const std::vector<MorseSegment>& table,
                              int totalFrames) noexcept;

    Atomics  m_atomics;
    Cached   m_cached;
    uint64_t m_cachedVersion{static_cast<uint64_t>(-1)};
    double   m_sampleRate{24000.0};

    // Audio-thread state.  Phase frame counter resets on every
    // phase transition.  Sine phase accumulator is per-phase too —
    // each phase starts from 0 so there's no carry-over click.
    int   m_phaseFrame{0};
    float m_sinePhase{0.0f};
    Phase m_currentPhase{Phase::Idle};

    // Independent state for the local sidetone mix path — runs on
    // the sidetone sink thread, not the TX audio thread, so it must
    // not share counters with process() above.  Reads the same atomic
    // phase but never writes it.
    int    m_localPhaseFrame{0};
    float  m_localSinePhase{0.0f};
    Phase  m_localCurrentPhase{Phase::Idle};
    double m_localSampleRate{48000.0};

    // Element tables for Morse "K" (intro) and "BK" (outro), built
    // once on prepare() and rebuilt whenever WPM changes.  Held in
    // the cache so the audio thread reads them lock-free between
    // recacheIfDirty() invocations.  The vectors themselves never
    // grow at audio time — capacity is reserved up front.
    std::vector<MorseSegment> m_morseIntroTable;
    std::vector<MorseSegment> m_morseOutroTable;

    PhaseCompleteCallback m_phaseCompleteCb;
};

} // namespace MasterSDR
