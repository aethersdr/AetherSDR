#pragma once

// AetherTX final peak control (RFC #4836): WDSP's SSB speech processor as
// the last client stage before Final Output. It bounds the envelope of the
// SSB signal the radio will build from this audio, not just the audio's
// sample peaks: bandpass, envelope clip ("Clip"), bandpass, CESSB overshoot
// control, bandpass, then the real part.
//
// The WDSP blocks run standalone behind
// third_party/wdsp/include/aether_wdsp_cessb.h. Built in prepare() under the
// FFTW planner lock, never on the audio thread.

#include <atomic>
#include <cstdint>

struct AetherCessb;

namespace AetherSDR {

class ClientCessb {
public:
    static constexpr int kBlockSize = 64;
    static constexpr int kTaps = 1024;          // each of the three bandpasses
    static constexpr float kMaxClipDb = 20.0f;
    static constexpr float kMinOutputDb = -12.0f;
    static constexpr float kMaxOutputDb = 0.0f;
    static constexpr float kDefaultLowHz = 150.0f;
    static constexpr float kDefaultHighHz = 2850.0f;

    ClientCessb();
    ~ClientCessb();
    ClientCessb(const ClientCessb&) = delete;
    ClientCessb& operator=(const ClientCessb&) = delete;

    // Owner thread, never concurrently with process(). Builds the blocks for
    // the current passband.
    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;
    void setClipDb(float db) noexcept;          // 0..20
    float clipDb() const noexcept;
    void setOvershootControl(bool on) noexcept; // CESSB
    bool overshootControl() const noexcept;
    void setOutputDb(float db) noexcept;        // -12..0, the envelope ceiling
    float outputDb() const noexcept;
    // Takes effect at the next prepare(). Keep it inside the radio's TX filter.
    void setPassband(float lowHz, float highHz) noexcept;
    float lowHz() const noexcept;
    float highHz() const noexcept;

    void process(float* interleaved, int frames, int channels) noexcept;
    void reset() noexcept;
    int latencyFrames() const noexcept;

private:
    void clearFifo() noexcept;

    double m_sampleRate{0.0};
    AetherCessb* m_block{nullptr};
    std::atomic<bool> m_enabled{false};
    std::atomic<float> m_clipDb{3.0f};
    std::atomic<bool> m_overshoot{true};
    std::atomic<float> m_outputDb{-1.0f};
    std::atomic<float> m_lowHz{kDefaultLowHz};
    std::atomic<float> m_highHz{kDefaultHighHz};
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_flushRequested{true};
    uint64_t m_appliedVersion{0};
    float m_outputGain{0.89125f};
    float m_inBlock[kBlockSize]{};
    float m_outBlock[kBlockSize]{};
    int m_fifoPos{0};
    std::atomic<int> m_latencyFrames{0};
};

}  // namespace AetherSDR
