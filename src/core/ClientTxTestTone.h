#pragma once

#include <atomic>
#include <cstdint>

namespace AetherSDR {

// 1 kHz sine test tone at the head of the TX path. When enabled, process()
// REPLACES the input (no mic sum) so the tone runs through the whole chain to
// VITA-49. Enable/freq/level are lock-free atomics.
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
    void process(float* interleaved, int frames, int channels) noexcept;

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

} // namespace AetherSDR
