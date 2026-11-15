#pragma once

// AetherTX peak control (RFC #4836's final peak control): WDSP's SSB speech
// processor as the last client stage before Final Output. It bounds the
// envelope of the SSB signal the radio builds from this audio, not just the
// audio's sample peaks: bandpass, envelope clip ("Clip"), bandpass, CESSB
// overshoot control, bandpass, then the real part. The radio's own TX filter
// must be wider than this stage's passband, or it reshapes the bounded
// envelope; AudioEngine keeps the passband inside it.
//
// The WDSP blocks run standalone behind
// third_party/wdsp/include/aether_wdsp_cessb.h. They are built and freed on
// the owner thread under the FFTW planner lock; the audio thread only swaps
// pointers, and a passband change runs the new block beside the old one and
// crossfades, so there is no gap.

#include <array>
#include <atomic>
#include <cstdint>

struct AetherCessb;

namespace AetherSDR {

class ClientCessb {
public:
    static constexpr int kBlockSize = 64;
    static constexpr int kTaps = 1024;          // each of the three bandpasses
    static constexpr float kMaxClipDb = 20.0f;
    static constexpr float kDefaultClipDb = 6.0f;
    static constexpr float kMinOutputDb = -12.0f;
    static constexpr float kMaxOutputDb = 0.0f;
    static constexpr float kDefaultOutputDb = -1.0f;
    static constexpr float kMinPassbandHz = 100.0f;
    static constexpr float kMaxPassbandHz = 9950.0f;  // a 10 kHz ESSB filter, less the margin
    static constexpr float kMinWidthHz = 500.0f;
    static constexpr float kDefaultLowHz = 150.0f;
    static constexpr float kDefaultHighHz = 2850.0f;
    // How far inside the radio's TX filter edges the passband sits.
    static constexpr float kRadioFilterMarginHz = 50.0f;

    ClientCessb();
    ~ClientCessb();
    ClientCessb(const ClientCessb&) = delete;
    ClientCessb& operator=(const ClientCessb&) = delete;

    // Owner thread, never concurrently with process().
    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;
    void setClipDb(float db) noexcept;          // 0..20
    float clipDb() const noexcept;
    void setOvershootControl(bool on) noexcept; // CESSB
    bool overshootControl() const noexcept;
    void setOutputDb(float db) noexcept;        // -12..0, the envelope ceiling
    float outputDb() const noexcept;

    // GUI thread; safe beside prepare() on the engine thread, since every
    // block hand-off is an atomic exchange. Clamped to 100..9950 Hz and at
    // least 500 Hz wide. Once prepared, builds the new block here and the
    // audio thread crossfades to it.
    void setPassband(float lowHz, float highHz);
    // The passband for a radio TX filter: its edges pulled in by the margin.
    void followRadioFilter(int radioLowHz, int radioHighHz);
    float lowHz() const noexcept;
    float highHz() const noexcept;

    void process(float* interleaved, int frames, int channels) noexcept;
    void reset() noexcept;
    int latencyFrames() const noexcept;

    // The largest SSB-envelope sample since the last call, in dBFS, after the
    // output ceiling (-120 when nothing has been processed).
    float takeEnvelopePeakDb() noexcept;

    // Owner thread: frees blocks the audio thread has finished with.
    void collectRetiredBlocks();
    static int liveBlockCount() noexcept;

private:
    static constexpr int kRetireSlots = 4;
    static constexpr int kCrossfadeSamples = 240;

    AetherCessb* buildBlock(float lowHz, float highHz) const;
    static void destroyBlock(AetherCessb* block);
    bool retire(AetherCessb* block) noexcept;
    bool hasRetireSlot() const noexcept;
    void applyParameters(AetherCessb* block) noexcept;
    void runBlock() noexcept;
    void clearFifo() noexcept;

    // Read by setPassband() on the GUI thread while prepare() may be writing
    // it on the audio engine's thread.
    std::atomic<double> m_sampleRate{0.0};
    std::atomic<bool> m_enabled{false};
    std::atomic<float> m_clipDb{kDefaultClipDb};
    std::atomic<bool> m_overshoot{true};
    std::atomic<float> m_outputDb{kDefaultOutputDb};
    std::atomic<float> m_lowHz{kDefaultLowHz};
    std::atomic<float> m_highHz{kDefaultHighHz};
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_flushRequested{true};
    uint64_t m_appliedVersion{0};
    float m_outputGain{0.89125f};

    AetherCessb* m_active{nullptr};
    AetherCessb* m_next{nullptr};
    std::atomic<AetherCessb*> m_pending{nullptr};
    std::array<std::atomic<AetherCessb*>, kRetireSlots> m_retired{};
    int m_nextSamples{0};

    std::array<float, kBlockSize> m_inBlock{};
    std::array<float, kBlockSize> m_outBlock{};
    int m_fifoPos{0};
    std::atomic<int> m_latencyFrames{0};
    std::atomic<float> m_envelopePeak{0.0f};
};

}  // namespace AetherSDR
