#pragma once

#include <atomic>
#include <cstdint>

namespace MasterSDR {

// 1 kHz sine test-tone generator, injected at the head of the TX
// audio path so users can verify the strip's processing chain
// without yelling into a mic.  Lock-free atomics on enable / freq /
// level so the UI can twiddle without ever touching audio-thread
// state outside of `process()`.
//
// When enabled, `process()` REPLACES the input buffer with a generated
// sine — not summed with mic input — so the user hears a clean tone
// running through the rest of the chain (Gate / EQ / Comp / DeEss /
// Tube / PUDU / Reverb / final-limiter / VITA-49).
class ClientTxTestTone {
public:
    ClientTxTestTone();
    ~ClientTxTestTone() = default;

    ClientTxTestTone(const ClientTxTestTone&)            = delete;
    ClientTxTestTone& operator=(const ClientTxTestTone&) = delete;

    void prepare(double sampleRate);

    void  setEnabled(bool on) noexcept;
    bool  isEnabled() const noexcept;

    void  setFrequencyHz(float hz) noexcept;        // 50..5000 Hz
    float frequencyHz() const noexcept;

    void  setLevelDb(float db) noexcept;            // -60..0 dBFS
    float levelDb() const noexcept;

    // Audio thread — overwrite int16 stereo samples with the
    // generated tone.  No-op if disabled.
    void process(int16_t* interleaved, int frames, int channels) noexcept;

    void reset() noexcept;

    double sampleRate() const noexcept { return m_sampleRate; }

private:
    void recacheIfDirty() noexcept;

    struct Atomics {
        std::atomic<bool>     enabled{false};
        std::atomic<float>    freqHz{1000.0f};
        std::atomic<float>    levelDb{-20.0f};
        std::atomic<uint64_t> version{0};
    };

    struct Cached {
        bool  enabled{false};
        float phaseInc{0.0f};   // radians per sample at current freq
        float ampLin{0.0f};     // 10^(level / 20)
    };

    Atomics m_atomics;
    Cached  m_cached;
    uint64_t m_cachedVersion{static_cast<uint64_t>(-1)};
    double   m_sampleRate{24000.0};
    float    m_phase{0.0f};
};

} // namespace MasterSDR
