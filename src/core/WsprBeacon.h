#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <QString>

namespace AetherSDR {

// Standard WSPR type-1 message encoder and sample-accurate continuous-phase
// 4-FSK generator. The UI prepares the 162 channel symbols; the audio thread
// consumes them without locks or allocation.
class WsprBeacon {
public:
    static constexpr int kSymbolCount = 162;
    static constexpr int kSampleRate = 24000;
    static constexpr int kFramesPerSymbol = 16384;
    static constexpr float kToneSpacingHz = 12000.0f / 8192.0f;
    static constexpr int kPreRollFrames = kSampleRate;
    static constexpr int64_t kMessageFrames =
        static_cast<int64_t>(kSymbolCount) * kFramesPerSymbol;
    static constexpr int64_t kTotalFrames = kPreRollFrames + kMessageFrames;
    static constexpr int kTransmitDurationMs =
        1000 + static_cast<int>(
            kMessageFrames * 1000 / kSampleRate);
    static constexpr int kMinimumInterlockTimeoutMs = 120000;

    static constexpr int64_t framesForElapsedNanoseconds(int64_t nanoseconds)
    {
        return nanoseconds * kSampleRate / 1000000000LL;
    }

    static constexpr bool isInterlockTimeoutSufficient(int timeoutMs)
    {
        return timeoutMs == 0 || timeoutMs >= kMinimumInterlockTimeoutMs;
    }

    using Symbols = std::array<uint8_t, kSymbolCount>;

    struct EncodeResult {
        Symbols symbols{};
        QString error;

        explicit operator bool() const { return error.isEmpty(); }
    };

    static EncodeResult encode(const QString& callsign,
                               const QString& locator,
                               int powerDbm);
    static bool isStandardPower(int powerDbm);

    void prepare(double sampleRate);
    void start(const Symbols& symbols, float centerHz, float levelDb,
               int preRollFrames = kPreRollFrames);
    void stop() noexcept;

    bool isActive() const noexcept;
    bool isComplete() const noexcept;
    int currentSymbol() const noexcept;

    // Audio thread. While active, always replaces the supplied buffer. After
    // the message completes it holds silence until stop(), preventing mic
    // leakage during the queued unkey edge.
    void process(int16_t* interleaved, int frames, int channels) noexcept;

private:
    void beginPendingTransmission() noexcept;

    std::array<std::atomic<uint8_t>, kSymbolCount> m_pendingSymbols{};
    std::atomic<float> m_pendingCenterHz{1500.0f};
    std::atomic<float> m_pendingLevelDb{-20.0f};
    std::atomic<int> m_pendingPreRollFrames{kSampleRate};
    std::atomic<uint64_t> m_version{0};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_complete{false};
    std::atomic<int> m_currentSymbol{-1};

    Symbols m_symbols{};
    uint64_t m_cachedVersion{0};
    double m_sampleRate{kSampleRate};
    float m_centerHz{1500.0f};
    float m_amplitude{0.1f};
    float m_phase{0.0f};
    int m_preRollRemaining{0};
    int m_symbolIndex{0};
    int m_framesIntoSymbol{0};
};

} // namespace AetherSDR
