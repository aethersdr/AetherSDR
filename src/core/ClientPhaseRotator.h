#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace AetherSDR {

// PAPR-reduction phase rotator — TX DSP chain stage (#2887).
//
// Voice is asymmetric: glottal pulses produce peaks taller in one polarity
// than the other. A compressor sees those tall peaks and pulls average gain
// down to clamp them. Cascading several second-order all-pass sections at
// staggered audio frequencies decorrelates the harmonic phase alignment
// that creates those tall peaks — peak amplitude drops 2–3 dB with no
// perceived loudness loss, so the compressor downstream has less to clamp
// and average TX power can run hotter. Standard broadcast technique
// (Optimod, Orban) adopted by Thetis for amateur SSB.
//
// Topology: cascade of N second-order all-pass (Direct Form I) filters at
// fixed staggered centre frequencies. All-pass means unit magnitude at
// every frequency (no spectral colouration) and only the phase response
// is rotated, decorrelating the constructive peak alignment.
//
// Default centres (300/700/1500/2500 Hz with optional 1000/2000 Hz add)
// are picked to cover the speech formant range without bunching.
//
// Thread model mirrors ClientGate: UI thread writes via set*() which
// update atomics + bump a version counter; the audio thread reads the
// version once per block and recaches. No locks, no allocations in
// process(), no exceptions.
class ClientPhaseRotator {
public:
    static constexpr int kMaxStages = 6;
    static constexpr int kDefaultStages = 4;

    ClientPhaseRotator();
    ~ClientPhaseRotator() = default;

    ClientPhaseRotator(const ClientPhaseRotator&)            = delete;
    ClientPhaseRotator& operator=(const ClientPhaseRotator&) = delete;

    // Main thread — call before first process() and on sample-rate change.
    void prepare(double sampleRate);

    // Main thread — global enable / bypass. Lock-free.
    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Number of cascaded all-pass sections.  Clamped to [0, kMaxStages].
    // 0 = pass-through. Higher counts give more peak reduction at the
    // cost of more phase rotation; 4 is the broadcast default.
    void setStages(int stages) noexcept;
    int  stages() const noexcept;

    // Audio thread — process in place.  channels must be 1 or 2.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — flush filter state (e.g. on TX start).
    void reset() noexcept;

    // UI thread — read-only snapshots of detector state for asymmetry
    // visualisation (peak+ / peak-) sampled per block.
    float inputPosPeakDb() const noexcept;
    float inputNegPeakDb() const noexcept;
    float outputPosPeakDb() const noexcept;
    float outputNegPeakDb() const noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    // Second-order all-pass section (Direct Form I).
    // Transfer function: H(z) = (a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2)
    // where a1, a2 are derived from centre frequency f0 and Q.
    struct AllPass {
        float a1{0.0f};
        float a2{0.0f};
        // Per-channel delay lines (x[n-1], x[n-2], y[n-1], y[n-2]).
        float x1[2]{0.0f, 0.0f};
        float x2[2]{0.0f, 0.0f};
        float y1[2]{0.0f, 0.0f};
        float y2[2]{0.0f, 0.0f};
    };

    void designSections() noexcept;
    void recacheIfDirty() noexcept;

    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<int>      stages{kDefaultStages};
        std::atomic<uint64_t> version{0};
    };

    struct Meters {
        std::atomic<float> inPosDb{-120.0f};
        std::atomic<float> inNegDb{-120.0f};
        std::atomic<float> outPosDb{-120.0f};
        std::atomic<float> outNegDb{-120.0f};
    };

    double   m_sampleRate{24000.0};
    Atomics  m_atomics;
    Meters   m_meters;

    // Audio-thread cache, refreshed on version bump.
    uint64_t m_lastVersion{0};
    int      m_cachedStages{kDefaultStages};

    // Six fixed staggered centre frequencies, Hz.  The cached stage count
    // selects how many of these participate (always from index 0 up).
    static constexpr float kCentreHz[kMaxStages] = {
        300.0f, 700.0f, 1500.0f, 2500.0f, 1000.0f, 2000.0f
    };
    static constexpr float kQ = 0.5f;

    std::array<AllPass, kMaxStages> m_sections;
};

} // namespace AetherSDR
