#pragma once

// AetherTX CFC: WDSP's Continuous Frequency Compressor (RFC #4836), a
// multiband compressor whose compression and post-EQ are curves over
// frequency rather than a fixed set of crossover bands.
//
// The WDSP block runs standalone, never through a TXA channel, behind the C
// facade in third_party/wdsp/include/aether_wdsp_cfc.h; no WDSP header
// reaches this one. Parameters are atomics the audio thread recaches once per
// block. The block is built (FFTW planning) on the calling thread, never the
// audio thread, and freed there too: the audio thread only swaps pointers.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

struct AetherCfc;

namespace AetherSDR {

class ClientCfc {
public:
    static constexpr int kMinBands = 2;
    static constexpr int kMaxBands = 10;
    static constexpr int kMinFftSize = 1024;
    static constexpr int kMaxFftSize = 16384;
    static constexpr int kDefaultFftSize = 1024;
    static constexpr int kBlockSize = 64;  // the FIFO between the chain and the block
    static constexpr int kDisplayPoints = 256;
    static constexpr float kMinHz = 10.0f;
    static constexpr float kMaxHz = 9990.0f;
    static constexpr float kMaxCompDb = 20.0f;
    static constexpr float kMaxEqDb = 20.0f;
    static constexpr float kMaxPrecompDb = 20.0f;
    static constexpr float kMaxPostGainDb = 20.0f;

    struct Band {
        float hz{1000.0f};
        float compDb{0.0f};
        float eqDb{0.0f};
        bool operator==(const Band&) const = default;
    };

    // create_txa()'s profile: 200/1k/2k/3k/4k Hz, compression 0/5/10/10/5 dB,
    // post-EQ +7 dB (with post-EQ itself off).
    [[nodiscard]] static std::vector<Band> defaultBands();
    // The nearest power of two in [kMinFftSize, kMaxFftSize].
    [[nodiscard]] static int snapFftSize(int size) noexcept;

    ClientCfc();
    ~ClientCfc();
    ClientCfc(const ClientCfc&) = delete;
    ClientCfc& operator=(const ClientCfc&) = delete;

    // Owner thread, never concurrently with process().
    void prepare(double sampleRate);

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Builds the new block on this thread; the audio thread crossfades to it
    // once it has filled, so the change has no gap.
    void setFftSize(int size);
    int fftSize() const noexcept;

    void setPrecompDb(float db) noexcept;
    float precompDb() const noexcept;
    void setPostEqEnabled(bool on) noexcept;
    bool postEqEnabled() const noexcept;
    void setPostGainDb(float db) noexcept;
    float postGainDb() const noexcept;

    // Clamped to the ranges above, sorted by frequency, kMinBands..kMaxBands
    // long (a shorter list is padded from defaultBands(), a longer one cut).
    void setBands(std::vector<Band> bands) noexcept;
    std::vector<Band> bands() const;

    // Audio thread, in place; channels 1 or 2 (the mic is duplicated mono, so
    // the left channel is processed and copied to the right).
    void process(float* interleaved, int frames, int channels) noexcept;
    // Any thread: the next process() starts from silence.
    void reset() noexcept;

    // The delay process() adds, in samples at the prepared rate.
    int latencyFrames() const noexcept;

    float inputPeakDb() const noexcept;
    float outputPeakDb() const noexcept;

    // Live compression for the editor, in dB at kDisplayPoints frequencies
    // from 0 Hz to Nyquist. Computed only while enabled.
    void setCompressionDisplayEnabled(bool on) noexcept;
    bool compressionDisplay(std::array<float, kDisplayPoints>& out) const noexcept;

    // Owner thread: frees the blocks the audio thread has finished with.
    // setFftSize() and the destructor do this too.
    void collectRetiredBlocks();

    // Blocks built and not yet freed, across every instance (test seam).
    static int liveBlockCount() noexcept;

private:
    static constexpr int kRetireSlots = 4;
    static constexpr int kCrossfadeSamples = 240;  // 5 ms at 48 kHz

    AetherCfc* buildBlock(int fftSize) const;
    static void destroyBlock(AetherCfc* block);
    bool retire(AetherCfc* block) noexcept;
    bool hasRetireSlot() const noexcept;
    void applyParameters(AetherCfc* block) noexcept;
    void recacheIfDirty() noexcept;
    void runBlock() noexcept;
    void clearFifo() noexcept;
    void bumpVersion() noexcept;

    double m_sampleRate{0.0};

    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_fftSize{kDefaultFftSize};
    std::atomic<float> m_precompDb{0.0f};
    std::atomic<bool> m_postEq{false};
    std::atomic<float> m_postGainDb{0.0f};
    std::atomic<int> m_bandCount{0};
    std::array<std::atomic<float>, kMaxBands * 3> m_bandValues{};
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_flushRequested{true};

    // Audio thread owns m_active and m_next; the GUI thread hands blocks over
    // through m_pending and takes them back through m_retired.
    AetherCfc* m_active{nullptr};
    AetherCfc* m_next{nullptr};
    std::atomic<AetherCfc*> m_pending{nullptr};
    std::array<std::atomic<AetherCfc*>, kRetireSlots> m_retired{};
    int m_nextSamples{0};
    std::atomic<int> m_activeFftSize{0};

    uint64_t m_appliedVersion{0};
    std::array<float, kBlockSize> m_inBlock{};
    std::array<float, kBlockSize> m_outBlock{};
    int m_fifoPos{0};

    std::atomic<float> m_inputPeakDb{-120.0f};
    std::atomic<float> m_outputPeakDb{-120.0f};

    std::atomic<bool> m_displayEnabled{false};
    mutable std::atomic<uint32_t> m_displaySeq{0};
    std::array<std::atomic<float>, kDisplayPoints> m_display{};
    std::array<float, kDisplayPoints> m_displayScratch{};
};

}  // namespace AetherSDR
