#pragma once

#include <atomic>
#include <cstdint>

namespace MasterSDR {

// Client-side tube saturator — TX DSP chain Phase 4 (#1661).  Per-
// sample soft-clipping waveshaper that adds tube-like harmonic
// warmth.  Modelled on Ableton's Dynamic Tube with three selectable
// curve flavours:
//
//   Model A — Soft tanh (broad, gentle)
//   Model B — Hard clip + tanh hybrid (odd harmonics, aggressive)
//   Model C — Asymmetric (bias-dominant, even harmonics, warm)
//
// The "Dynamic" part: an envelope follower on the input modulates
// drive so quiet passages stay clean while loud sections saturate
// more.  Envelope amount + Attack + Release shape that behaviour.
// A pre-tilt "Tone" filter shifts which part of the spectrum gets
// pushed into the nonlinearity, and a parallel Dry/Wet mix lets
// users blend processed + dry.
//
// Thread model mirrors ClientComp / ClientGate / ClientDeEss:
// UI thread writes atomics + bumps a version counter; the audio
// thread reads the version once per block and recaches derived
// values.  No locks, no allocations, no exceptions.
class ClientTube {
public:
    enum class Model : uint8_t {
        A = 0,   // soft tanh
        B = 1,   // hard clip + tanh hybrid
        C = 2,   // asymmetric
    };

    ClientTube();
    ~ClientTube() = default;

    ClientTube(const ClientTube&)            = delete;
    ClientTube& operator=(const ClientTube&) = delete;

    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    void  setModel(Model m) noexcept;
    Model model() const noexcept;

    // Core drive + asymmetry.
    void  setDriveDb(float db) noexcept;             // 0..24 dB
    float driveDb() const noexcept;
    void  setBiasAmount(float v) noexcept;           // 0..1 (0 = symmetric)
    float biasAmount() const noexcept;

    // Pre-shaper tone: simple tilt filter.  -1 = all dark (cut HF
    // before saturation), +1 = all bright (boost HF into the
    // shaper), 0 = flat.
    void  setTone(float v) noexcept;                 // -1..+1
    float tone() const noexcept;

    // Output trim + parallel mix.
    void  setOutputGainDb(float db) noexcept;        // -24..+24 dB
    float outputGainDb() const noexcept;
    void  setDryWet(float v) noexcept;               // 0..1 (0 = dry, 1 = wet)
    float dryWet() const noexcept;

    // Dynamic drive modulation via input envelope.  Bipolar:
    //   0     = static drive (no modulation)
    //   +1    = louder input pushes drive higher (normal)
    //   -1    = louder input REDUCES drive (reverse / ducking)
    // Reverse envelope is a classic tube-preamp trick for keeping
    // loud passages clean while adding harmonic warmth on quiet ones.
    void  setEnvelopeAmount(float v) noexcept;       // -1..+1
    float envelopeAmount() const noexcept;
    void  setAttackMs(float ms) noexcept;            // 0.1..30 ms
    float attackMs() const noexcept;
    void  setReleaseMs(float ms) noexcept;           // 10..500 ms
    float releaseMs() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush envelope + tone-filter state.
    void reset() noexcept;

    // UI thread — read-only snapshots.
    float inputPeakDb() const noexcept;
    float outputPeakDb() const noexcept;
    float driveAppliedDb() const noexcept;   // dynamic instantaneous drive

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<uint8_t>  model{static_cast<uint8_t>(Model::A)};
        std::atomic<float>    driveDb{0.0f};
        std::atomic<float>    biasAmount{0.0f};
        std::atomic<float>    tone{0.0f};
        std::atomic<float>    outputGainDb{0.0f};
        std::atomic<float>    dryWet{1.0f};
        std::atomic<float>    envelopeAmount{0.0f};
        std::atomic<float>    attackMs{5.0f};
        std::atomic<float>    releaseMs{35.0f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        uint8_t model{0};
        float baseDriveLin{1.0f};      // 10^(driveDb/20)
        float bias{0.0f};
        float tone{0.0f};
        float outputLin{1.0f};
        float dryWet{1.0f};
        float envAmount{0.0f};
        float attackCoeff{0.0f};
        float releaseCoeff{0.0f};
        // One-pole tilt coefficient: y = x + a*(x - y_prev)
        float toneCoeff{0.0f};
    };

    struct Meters {
        std::atomic<float> inputPeakDb{-120.0f};
        std::atomic<float> outputPeakDb{-120.0f};
        std::atomic<float> driveAppliedDb{0.0f};
    };

    void recacheIfDirty() noexcept;
    float shape(float x) const noexcept;   // waveshaper per Model + bias

    double m_sampleRate{24000.0};
    Atomics m_atomics;
    Cached  m_cached;
    Meters  m_meters;

    // Audio-thread state.
    uint64_t m_lastVersion{0};
    float    m_envLin{0.0f};          // input peak envelope, linear
    // One-pole tilt state (tone filter) per channel.  Implemented as
    // a simple HP/LP blend driven by m_cached.tone — positive tone
    // brightens, negative darkens before the shaper.
    float    m_toneStateL{0.0f};
    float    m_toneStateR{0.0f};
};

} // namespace MasterSDR
