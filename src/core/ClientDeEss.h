#pragma once

#include <atomic>
#include <cstdint>

namespace MasterSDR {

// Client-side de-esser — TX DSP chain Phase 3 (#1661).  Sibilant
// suppression via sidechain-filtered dynamics: the input is split
// through a bandpass filter (2–10 kHz) whose output drives an
// envelope detector; when the detector crosses threshold we apply
// broadband attenuation (capped at `amount` dB) to the full signal.
//
// Single-band design.  Classic approach, tracks modern plugins like
// Ableton's DeEsser and FabFilter's Pro-DS in structure.  Future
// phases could add split-band (only attenuate HF), stereo-linked
// vs. dual-mono detection, or listen-to-sidechain monitoring.
//
// Thread model mirrors ClientComp / ClientGate: UI thread writes
// atomics + bumps a version counter; the audio thread reads the
// version once per block and recaches coefficients.  No locks,
// no allocations in process(), no exceptions.
class ClientDeEss {
public:
    ClientDeEss();
    ~ClientDeEss() = default;

    ClientDeEss(const ClientDeEss&)            = delete;
    ClientDeEss& operator=(const ClientDeEss&) = delete;

    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Sidechain band: bandpass centre frequency (Hz) and Q.  Centre
    // usually 4–7 kHz for voice sibilants; Q around 1.5–3 keeps the
    // band tight enough to miss low-end speech content.
    void  setFrequencyHz(float hz) noexcept;         // 1000..12000 Hz
    float frequencyHz() const noexcept;
    void  setQ(float q) noexcept;                    // 0.5..5.0
    float q() const noexcept;

    // Gate-like dynamics on the sidechain envelope.  Threshold is
    // specified in dBFS; amount is the MAX attenuation in dB (a
    // negative value, e.g. -6 dB).  Attack/release default tight
    // since sibilants are fast events.
    void  setThresholdDb(float db) noexcept;         // -60..0 dB
    float thresholdDb() const noexcept;
    void  setAmountDb(float db) noexcept;            // -24..0 dB (max reduction)
    float amountDb() const noexcept;
    void  setAttackMs(float ms) noexcept;            // 0.1..30 ms
    float attackMs() const noexcept;
    void  setReleaseMs(float ms) noexcept;           // 10..500 ms
    float releaseMs() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush all state (envelope + biquad memory).
    void reset() noexcept;

    // UI thread — read-only snapshots of the latest detector state.
    float inputPeakDb() const noexcept;             // full-band input peak
    float sidechainPeakDb() const noexcept;         // HF-band sidechain peak
    float gainReductionDb() const noexcept;         // ≤ 0 dB

    double sampleRate() const noexcept { return m_sampleRate; }

    // Public because the cpp-local biquad helper takes references to
    // these.  Not part of the user-facing API.
    struct BiquadCoef {
        float b0{1.0f}, b1{0.0f}, b2{0.0f};
        float a1{0.0f}, a2{0.0f};
    };
    struct BiquadState {
        float z1{0.0f}, z2{0.0f};
    };

private:
    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<float>    frequencyHz{6000.0f};
        std::atomic<float>    q{2.0f};
        std::atomic<float>    thresholdDb{-30.0f};
        std::atomic<float>    amountDb{-6.0f};
        std::atomic<float>    attackMs{1.0f};
        std::atomic<float>    releaseMs{100.0f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        BiquadCoef bp{};
        float thresholdDb{-30.0f};
        float amountDb{-6.0f};       // negative — max attenuation
        float attackCoeff{0.0f};
        float releaseCoeff{0.0f};
    };

    struct Meters {
        std::atomic<float> inputPeakDb{-120.0f};
        std::atomic<float> sidechainPeakDb{-120.0f};
        std::atomic<float> gainReductionDb{0.0f};
    };

    void recacheIfDirty() noexcept;
    float staticCurveGainDb(float envDb) const noexcept;

    double m_sampleRate{24000.0};
    Atomics m_atomics;
    Cached  m_cached;
    Meters  m_meters;

    // Audio-thread state — accessed only from process().
    uint64_t m_lastVersion{0};
    BiquadState m_bpL{};
    BiquadState m_bpR{};
    float m_envLin{0.0f};
};

} // namespace MasterSDR
