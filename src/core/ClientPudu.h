#pragma once

#include <atomic>
#include <cstdint>

namespace MasterSDR {

// Client-side TX exciter — TX DSP chain Phase 5 (#1661).  The
// centrepiece of the PooDoo Audio™ chain.  Two parallel bands ("Poo"
// on the low end and "Doo" on the high end), each with a mode-
// selectable algorithm:
//
//   Mode A (Aphex):
//     Doo = classic Aural Exciter — HPF → VGA → asymmetric diode
//           soft-clip → DC block → attenuation.  Produces both odd
//           and even harmonics.  Warm, "3-D" character.
//     Poo = Big Bottom topology — LPF → envelope-follower dynamic
//           EQ → soft saturation → attenuation.  Adds harmonic
//           content to the low band, enhances perceived weight.
//
//   Mode B (Behringer SX3040):
//     Doo = symmetric soft-saturation HPF → drive → tanh → mix.
//           Odd harmonics only, tighter and brighter.
//     Poo = frequency-selective compressor + 2nd-order all-pass
//           phase rotator.  No harmonic content; dynamic transient
//           emphasis with phase-aligned re-injection into dry.
//
// Both modes expose the same six user knobs — only the DSP blocks
// under the hood swap when Mode toggles:
//
//   Poo: Drive (0..24 dB)       -- saturation depth (A) / compressor intensity (B)
//        Tune  (50..160 Hz)     -- LPF corner
//        Mix   (0..1)           -- wet blend into dry sum
//   Doo: Tune      (1..10 kHz)  -- HPF corner
//        Harmonics (0..24 dB)   -- drive into the nonlinearity
//        Mix       (0..1)       -- wet blend into dry sum
//
// Thread model mirrors the other DSP modules: UI thread writes
// atomics + bumps a version counter; audio thread reads the version
// once per block and recaches derived values.  No locks, no
// allocations in process(), no exceptions.
class ClientPudu {
public:
    enum class Mode : uint8_t {
        Aphex     = 0,   // Aural Exciter + Big Bottom
        Behringer = 1,   // SX 3040 Sonic Exciter
    };

    ClientPudu();
    ~ClientPudu() = default;

    ClientPudu(const ClientPudu&)            = delete;
    ClientPudu& operator=(const ClientPudu&) = delete;

    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    void  setMode(Mode m) noexcept;
    Mode  mode() const noexcept;

    // Poo (low band).
    void  setPooDriveDb(float db) noexcept;          // 0..24 dB
    float pooDriveDb() const noexcept;
    void  setPooTuneHz(float hz) noexcept;           // 50..160 Hz
    float pooTuneHz() const noexcept;
    void  setPooMix(float v) noexcept;               // 0..1
    float pooMix() const noexcept;

    // Doo (high band).
    void  setDooTuneHz(float hz) noexcept;           // 1000..10000 Hz
    float dooTuneHz() const noexcept;
    void  setDooHarmonicsDb(float db) noexcept;      // 0..24 dB
    float dooHarmonicsDb() const noexcept;
    void  setDooMix(float v) noexcept;               // 0..1
    float dooMix() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush biquad + envelope + DC-block state.
    void reset() noexcept;

    // UI thread — read-only snapshots.
    float inputPeakDb() const noexcept;
    float outputPeakDb() const noexcept;
    // Post-nonlinearity RMS in dB.  Drives the PooDoo logo pulse in
    // the applet — dim at silence, bright when the exciter is
    // actively adding content.
    float wetRmsDb() const noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

    // Public because the cpp-local biquad helper takes references.
    // Not part of the user-facing API.
    struct BiquadCoef { float b0{1}, b1{0}, b2{0}, a1{0}, a2{0}; };
    struct BiquadState { float z1{0}, z2{0}; };

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<uint8_t>  mode{static_cast<uint8_t>(Mode::Aphex)};
        std::atomic<float>    pooDriveDb{0.0f};
        std::atomic<float>    pooTuneHz{100.0f};
        std::atomic<float>    pooMix{0.5f};
        std::atomic<float>    dooTuneHz{5000.0f};
        std::atomic<float>    dooHarmonicsDb{6.0f};
        std::atomic<float>    dooMix{0.5f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        uint8_t mode{0};
        // HF path.
        BiquadCoef hpf{};
        float      dooDriveLin{1.0f};   // 10^(harmonics/20)
        float      dooMix{0.5f};
        // LF path.
        BiquadCoef lpf{};
        BiquadCoef allpass{};            // used in Behringer mode
        float      pooDriveLin{1.0f};
        float      pooMix{0.5f};
        // LF dynamics (both modes, different coefficients).
        float      envAttackCoeff{0.0f};
        float      envReleaseCoeff{0.0f};
    };

    struct Meters {
        std::atomic<float> inputPeakDb{-120.0f};
        std::atomic<float> outputPeakDb{-120.0f};
        std::atomic<float> wetRmsDb{-120.0f};
    };

    void recacheIfDirty() noexcept;

    // Per-channel state bundles so stereo maintains independent
    // biquad memories without tangling the process() loop.
    struct ChannelState {
        BiquadState hpf;
        BiquadState lpf;
        BiquadState allpass;
        // Single-pole DC block on the Doo path (Aphex mode only —
        // needed because one-sided clipping introduces DC offset).
        float dcX1{0.0f};
        float dcY1{0.0f};
    };

    double  m_sampleRate{24000.0};
    Atomics m_atomics;
    Cached  m_cached;
    Meters  m_meters;

    // Audio-thread state.
    uint64_t     m_lastVersion{0};
    ChannelState m_chL;
    ChannelState m_chR;
    // Envelope followers for the LF band.  Shared across modes —
    // Aphex mode treats them as a dynamic-EQ level detector; Behringer
    // mode treats them as a compressor sidechain.
    float        m_lfEnvLin{0.0f};
};

} // namespace MasterSDR
