#include "core/dsp/WdspChannel.h"

#include <aether_wdsp.h>

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace {

constexpr int kWdspChannelCount = 32;
constexpr int kRxChannelType = 0;
constexpr int kTxChannelType = 1;

std::mutex g_channelMutex;
std::mutex g_setupMutex;
std::array<bool, kWdspChannelCount> g_channelsInUse {};

int acquireChannelId()
{
    const std::scoped_lock lock(g_channelMutex);
    for (int channel = 0; channel < kWdspChannelCount; ++channel) {
        if (!g_channelsInUse[static_cast<std::size_t>(channel)]) {
            g_channelsInUse[static_cast<std::size_t>(channel)] = true;
            return channel;
        }
    }
    return -1;
}

void releaseChannelId(int channel)
{
    if (channel < 0 || channel >= kWdspChannelCount) {
        return;
    }
    const std::scoped_lock lock(g_channelMutex);
    g_channelsInUse[static_cast<std::size_t>(channel)] = false;
}

void setError(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::unique_ptr<WdspChannel> WdspChannel::create(const Config& config,
                                                 std::string* error) noexcept
{
    if (!validateConfig(config, error)) {
        return nullptr;
    }
    if (GetWDSPVersion() != 200) {
        setError(error, "The linked WDSP library is not version 2.00");
        return nullptr;
    }

    const int channelId = acquireChannelId();
    if (channelId < 0) {
        setError(error, "All WDSP channel slots are in use");
        return nullptr;
    }

    std::unique_ptr<WdspChannel> channel(new (std::nothrow) WdspChannel(channelId, config));
    if (!channel) {
        releaseChannelId(channelId);
        setError(error, "Could not allocate the WDSP channel owner");
        return nullptr;
    }
    channel->open();
    return channel;
}

WdspChannel::WdspChannel(int channelId, const Config& config) noexcept
    : m_channelId(channelId)
    , m_config(config)
    , m_outputBlockSize(computeOutputBlockSize(config))
{
}

WdspChannel::~WdspChannel()
{
    m_controlOperation.store(true, std::memory_order_seq_cst);
    while (m_callbacksInFlight.load(std::memory_order_seq_cst) != 0) {
        // Yield to the real-time thread we are draining rather than burning a
        // core; avoids priority inversion if it was preempted mid-fexchange2.
        std::this_thread::yield();
    }
    close();
    releaseChannelId(m_channelId);
}

WdspChannel::ProcessResult WdspChannel::processIq(std::span<const float> inputI,
                                                  std::span<const float> inputQ,
                                                  std::span<float> outputLeft,
                                                  std::span<float> outputRight) noexcept
{
    if (inputI.size() != m_config.inputBlockSize || inputQ.size() != inputI.size() ||
        outputLeft.size() != m_outputBlockSize || outputRight.size() != outputLeft.size()) {
        return ProcessResult::InvalidBuffer;
    }
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        return ProcessResult::Busy;
    }

    m_callbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return ProcessResult::Busy;
    }

    const uint64_t allocationsBefore = wdspPortAllocationSequence();
    int wdspError = 0;
    fexchange2(m_channelId,
               const_cast<float*>(inputI.data()),
               const_cast<float*>(inputQ.data()),
               outputLeft.data(), outputRight.data(), &wdspError);
    const uint64_t allocationsAfter = wdspPortAllocationSequence();
    m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);

    if (allocationsAfter != allocationsBefore) {
        return ProcessResult::AllocationViolation;
    }
    if (wdspError == -2) {
        return ProcessResult::Underrun;
    }
    if (wdspError != 0) {
        return ProcessResult::EngineError;
    }
    return ProcessResult::Ok;
}

bool WdspChannel::reconfigure(const Config& config, std::string* error) noexcept
{
    if (!validateConfig(config, error) || !beginControlOperation()) {
        if (error != nullptr && error->empty()) {
            *error = "WDSP channel is processing audio";
        }
        return false;
    }

    close();
    m_config = config;
    m_outputBlockSize = computeOutputBlockSize(m_config);
    open();
    endControlOperation();
    return true;
}

bool WdspChannel::setMode(Mode mode) noexcept
{
    if (!beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_config.direction == Direction::Receive) {
            SetRXAMode(m_channelId, wdspMode(mode));
        } else {
            SetTXAMode(m_channelId, wdspMode(mode));
        }
    }
    m_config.mode = mode;
    endControlOperation();
    return true;
}

bool WdspChannel::setFilter(double lowHz, double highHz) noexcept
{
    if (!std::isfinite(lowHz) || !std::isfinite(highHz) || lowHz >= highHz ||
        !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_config.direction == Direction::Receive) {
            SetRXABandpassFreqs(m_channelId, lowHz, highHz);
        } else {
            SetTXABandpassFreqs(m_channelId, lowHz, highHz);
        }
    }
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    endControlOperation();
    return true;
}

std::size_t WdspChannel::outputBlockSize() const noexcept
{
    return m_outputBlockSize;
}

std::size_t WdspChannel::computeOutputBlockSize(const Config& config) noexcept
{
    // Exact: validateConfig() guarantees inputSampleRate > 0 and that
    // inputBlockSize * outputSampleRate is a whole multiple of inputSampleRate.
    return config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate) /
           static_cast<std::size_t>(config.inputSampleRate);
}

uint64_t WdspChannel::allocationSequenceForTest() noexcept
{
    return wdspPortAllocationSequence();
}

uint64_t WdspChannel::outstandingAllocationsForTest() noexcept
{
    return wdspPortOutstandingAllocations();
}

bool WdspChannel::validateConfig(const Config& config, std::string* error) noexcept
{
    if (config.inputBlockSize == 0 || config.dspBlockSize == 0 ||
        config.inputBlockSize > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        config.dspBlockSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        setError(error, "WDSP block sizes must be positive 32-bit values");
        return false;
    }
    if (config.inputSampleRate <= 0 || config.dspSampleRate <= 0 ||
        config.outputSampleRate <= 0) {
        setError(error, "WDSP sample rates must be positive");
        return false;
    }
    if ((config.inputSampleRate % config.dspSampleRate != 0 &&
         config.dspSampleRate % config.inputSampleRate != 0) ||
        (config.outputSampleRate % config.dspSampleRate != 0 &&
         config.dspSampleRate % config.outputSampleRate != 0) ||
        (config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate)) %
            static_cast<std::size_t>(config.inputSampleRate) != 0) {
        setError(error, "WDSP rates and block sizes must have integral ratios");
        return false;
    }
    if (!std::isfinite(config.filterLowHz) || !std::isfinite(config.filterHighHz) ||
        config.filterLowHz >= config.filterHighHz) {
        setError(error, "WDSP filter edges are invalid");
        return false;
    }
    if (config.direction == Direction::Transmit && config.mode == Mode::Wbfm) {
        setError(error, "WDSP TX does not define a WBFM mode");
        return false;
    }
    return true;
}

int WdspChannel::wdspMode(Mode mode) noexcept
{
    return static_cast<int>(mode);
}

void WdspChannel::open() noexcept
{
    const std::scoped_lock setupLock(g_setupMutex);
    OpenChannel(m_channelId,
                static_cast<int>(m_config.inputBlockSize),
                static_cast<int>(m_config.dspBlockSize),
                m_config.inputSampleRate,
                m_config.dspSampleRate,
                m_config.outputSampleRate,
                m_config.direction == Direction::Receive ? kRxChannelType : kTxChannelType,
                1,
                0.0, 0.0, 0.0, 0.0,
                m_config.blockForOutput ? 1 : 0);
    if (m_config.direction == Direction::Receive) {
        SetRXAMode(m_channelId, wdspMode(m_config.mode));
        SetRXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
        SetRXAAGCMode(m_channelId, m_config.agcMode);
        SetRXAAGCTop(m_channelId, m_config.maximumAgcGainDb);
    } else {
        SetTXAMode(m_channelId, wdspMode(m_config.mode));
        SetTXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
    }
    m_open = true;
}

void WdspChannel::close() noexcept
{
    if (!m_open) {
        return;
    }
    const std::scoped_lock setupLock(g_setupMutex);
    CloseChannel(m_channelId);
    m_open = false;
}

bool WdspChannel::beginControlOperation() noexcept
{
    bool expected = false;
    if (!m_controlOperation.compare_exchange_strong(expected, true,
                                                    std::memory_order_seq_cst)) {
        return false;
    }
    if (m_callbacksInFlight.load(std::memory_order_seq_cst) != 0) {
        m_controlOperation.store(false, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

void WdspChannel::endControlOperation() noexcept
{
    m_controlOperation.store(false, std::memory_order_seq_cst);
}
