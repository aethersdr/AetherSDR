#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

// Owns one complete WDSP channel and hides WDSP's process-global numeric
// channel table. Construction, reconfiguration, and filter changes are control-
// thread operations; processIq() is the allocation-free real-time operation.
class WdspChannel final
{
public:
    enum class Direction
    {
        Receive,
        Transmit
    };

    enum class Mode
    {
        Lsb,
        Usb,
        Dsb,
        Cwl,
        Cwu,
        Fm,
        Am,
        Digu,
        Spec,
        Digl,
        Sam,
        Drm,
        Wbfm
    };

    struct Config
    {
        Direction direction = Direction::Receive;
        std::size_t inputBlockSize = 1024;
        std::size_t dspBlockSize = 1024;
        int inputSampleRate = 48000;
        int dspSampleRate = 48000;
        int outputSampleRate = 48000;
        Mode mode = Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        int agcMode = 3;
        double maximumAgcGainDb = 120.0;
        bool blockForOutput = false;
    };

    enum class ProcessResult
    {
        Ok,
        Underrun,
        Busy,
        InvalidBuffer,
        AllocationViolation,
        EngineError
    };

    static std::unique_ptr<WdspChannel> create(const Config& config,
                                               std::string* error = nullptr) noexcept;

    ~WdspChannel();

    WdspChannel(const WdspChannel&) = delete;
    WdspChannel& operator=(const WdspChannel&) = delete;
    WdspChannel(WdspChannel&&) = delete;
    WdspChannel& operator=(WdspChannel&&) = delete;

    ProcessResult processIq(std::span<const float> inputI,
                            std::span<const float> inputQ,
                            std::span<float> outputLeft,
                            std::span<float> outputRight) noexcept;

    // The caller must stop feeding processIq() before a control operation.
    bool reconfigure(const Config& config, std::string* error = nullptr) noexcept;
    bool setMode(Mode mode) noexcept;
    bool setFilter(double lowHz, double highHz) noexcept;

    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] std::size_t outputBlockSize() const noexcept;
    [[nodiscard]] int channelIdForTest() const noexcept { return m_channelId; }

    static uint64_t allocationSequenceForTest() noexcept;
    static uint64_t outstandingAllocationsForTest() noexcept;

private:
    explicit WdspChannel(int channelId, const Config& config) noexcept;

    static bool validateConfig(const Config& config, std::string* error) noexcept;
    static int wdspMode(Mode mode) noexcept;

    void open() noexcept;
    void close() noexcept;
    bool beginControlOperation() noexcept;
    void endControlOperation() noexcept;

    int m_channelId = -1;
    Config m_config;
    std::atomic<unsigned> m_callbacksInFlight {0};
    std::atomic<bool> m_controlOperation {false};
    bool m_open = false;
};
