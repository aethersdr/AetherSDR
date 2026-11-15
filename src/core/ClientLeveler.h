#pragma once

// AetherTX LVLR: WDSP's TX leveler (RFC #4836), a slow gain rider that
// brings a quiet or distant voice up toward full scale, at most Max gain,
// before the faster stages see it.
//
// The WDSP block runs standalone, never through a TXA channel, behind the C
// facade in third_party/wdsp/include/aether_wdsp_leveler.h. Parameters are
// atomics the audio thread applies once per block (allocation-free); the
// block is built and freed in prepare() and the destructor, never on the
// audio thread.

#include <atomic>
#include <cstdint>

struct AetherLeveler;

namespace AetherSDR {

class ClientLeveler {
public:
    static constexpr int kBlockSize = 64;  // the FIFO between the chain and the block
    static constexpr float kMaxGainDb = 20.0f;
    static constexpr float kMinAttackMs = 1.0f;
    static constexpr float kMaxAttackMs = 10.0f;  // wcpAGC's ring: MAX_TAU_ATTACK
    static constexpr float kMinDecayMs = 1.0f;
    static constexpr float kMaxDecayMs = 5000.0f;
    static constexpr float kMinHangMs = 10.0f;    // below this, hang is off
    static constexpr float kMaxHangMs = 5000.0f;
    // create_txa()'s leveler.
    static constexpr float kDefaultMaxGainDb = 5.0f;
    static constexpr float kDefaultAttackMs = 1.0f;
    static constexpr float kDefaultDecayMs = 500.0f;

    ClientLeveler();
    ~ClientLeveler();
    ClientLeveler(const ClientLeveler&) = delete;
    ClientLeveler& operator=(const ClientLeveler&) = delete;

    // Owner thread, never concurrently with process().
    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    void setMaxGainDb(float db) noexcept;   // 0..20
    float maxGainDb() const noexcept;
    void setAttackMs(float ms) noexcept;    // 1..10
    float attackMs() const noexcept;
    void setDecayMs(float ms) noexcept;     // 1..5000
    float decayMs() const noexcept;
    void setHangMs(float ms) noexcept;      // 0 (off) or 10..5000
    float hangMs() const noexcept;

    // Audio thread, in place; channels 1 or 2 (left processed, copied right).
    void process(float* interleaved, int frames, int channels) noexcept;
    // Any thread: the next process() starts from silence.
    void reset() noexcept;

    // The delay process() adds: the FIFO plus the look-ahead the attack sets.
    int latencyFrames() const noexcept;
    // The gain the leveler applied at the end of the last block, in dB.
    float gainDb() const noexcept;

private:
    void applyParameters() noexcept;
    void clearFifo() noexcept;

    double m_sampleRate{0.0};
    AetherLeveler* m_block{nullptr};

    std::atomic<bool> m_enabled{false};
    std::atomic<float> m_maxGainDb{kDefaultMaxGainDb};
    std::atomic<float> m_attackMs{kDefaultAttackMs};
    std::atomic<float> m_decayMs{kDefaultDecayMs};
    std::atomic<float> m_hangMs{0.0f};
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_flushRequested{true};
    uint64_t m_appliedVersion{0};

    float m_inBlock[kBlockSize]{};
    float m_outBlock[kBlockSize]{};
    int m_fifoPos{0};

    std::atomic<int> m_latencyFrames{kBlockSize};
    std::atomic<float> m_gainDb{0.0f};
};

}  // namespace AetherSDR
