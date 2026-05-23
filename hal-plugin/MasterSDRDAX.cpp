// MasterSDR DAX — Core Audio HAL Audio Server Plug-In
//
// Creates 4 virtual audio output devices ("MasterSDR DAX 1" through "MasterSDR DAX 4")
// for receiving DAX audio from the radio, plus 1 virtual input device ("MasterSDR TX")
// for sending TX audio to the radio.
//
// Each device reads/writes PCM audio via a POSIX shared memory ring buffer shared
// with MasterSDR's VirtualAudioBridge.
//
// Format: stereo float32, 24 kHz (matches FlexRadio DAX native rate).

#include <aspl/Driver.hpp>
#include <aspl/DriverRequestHandler.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/Context.hpp>
#include <aspl/Tracer.hpp>

#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>

// ── Shared memory layout — must match VirtualAudioBridge.h ──────────────────

struct DaxShmBlock {
    std::atomic<uint32_t> writePos;
    std::atomic<uint32_t> readPos;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t active;
    uint32_t reserved[3];
    static constexpr uint32_t RING_SIZE = 24000 * 2 * 2;  // ~2 sec @ 24kHz stereo
    float ringBuffer[RING_SIZE];
};

// ── DAX RX Handler: reads from shared memory → output to apps ───────────────

class DaxRxHandler : public aspl::IORequestHandler {
public:
    explicit DaxRxHandler(int channel)
        : m_channel(channel)
    {}

    ~DaxRxHandler() override
    {
        unmapShm();
    }

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override
    {
        auto* dst = static_cast<float*>(bytes);
        const UInt32 totalSamples = bytesCount / sizeof(float);

        if (!ensureShm()) {
            std::memset(dst, 0, bytesCount);
            return;
        }

        auto* block = m_shmBlock;
        if (!block->active) {
            std::memset(dst, 0, bytesCount);
            return;
        }

        uint32_t rp = block->readPos.load(std::memory_order_acquire);
        uint32_t wp = block->writePos.load(std::memory_order_acquire);

        uint32_t available = wp - rp;
        uint32_t toRead = std::min(available, totalSamples);

        for (uint32_t i = 0; i < toRead; ++i) {
            dst[i] = block->ringBuffer[rp % DaxShmBlock::RING_SIZE];
            ++rp;
        }

        if (toRead < totalSamples) {
            std::memset(dst + toRead, 0, (totalSamples - toRead) * sizeof(float));
        }

        block->readPos.store(rp, std::memory_order_release);
    }

private:
    bool ensureShm()
    {
        if (m_shmBlock) return true;

        // Retry periodically — MasterSDR may not have created the segment yet.
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastRetry < std::chrono::seconds(1)) return false;
        m_lastRetry = now;

        char name[64];
        snprintf(name, sizeof(name), "/MasterSDR-dax-%d", m_channel);

        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return false;

        void* ptr = mmap(nullptr, sizeof(DaxShmBlock), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) return false;

        m_shmBlock = static_cast<DaxShmBlock*>(ptr);
        return true;
    }

    void unmapShm()
    {
        if (m_shmBlock) {
            munmap(m_shmBlock, sizeof(DaxShmBlock));
            m_shmBlock = nullptr;
        }
    }

    int m_channel{1};
    DaxShmBlock* m_shmBlock{nullptr};
    std::chrono::steady_clock::time_point m_lastRetry{};
};

// ── DAX TX Handler: receives audio from apps → writes to shared memory ──────

class DaxTxHandler : public aspl::IORequestHandler {
public:
    DaxTxHandler() = default;

    ~DaxTxHandler() override
    {
        unmapShm();
    }

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                            Float64 zeroTimestamp,
                            Float64 timestamp,
                            const void* bytes,
                            UInt32 bytesCount) override
    {
        if (!ensureShm()) return;

        auto* block = m_shmBlock;
        const auto* src = static_cast<const float*>(bytes);
        const UInt32 totalSamples = bytesCount / sizeof(float);

        uint32_t wp = block->writePos.load(std::memory_order_acquire);

        for (uint32_t i = 0; i < totalSamples; ++i) {
            block->ringBuffer[wp % DaxShmBlock::RING_SIZE] = src[i];
            ++wp;
        }

        block->writePos.store(wp, std::memory_order_release);
        block->active = 1;
    }

private:
    bool ensureShm()
    {
        if (m_shmBlock) return true;

        auto now = std::chrono::steady_clock::now();
        if (now - m_lastRetry < std::chrono::seconds(1)) return false;
        m_lastRetry = now;

        int fd = shm_open("/MasterSDR-dax-tx", O_RDWR, 0666);
        if (fd < 0) return false;

        void* ptr = mmap(nullptr, sizeof(DaxShmBlock), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) return false;

        m_shmBlock = static_cast<DaxShmBlock*>(ptr);
        return true;
    }

    void unmapShm()
    {
        if (m_shmBlock) {
            munmap(m_shmBlock, sizeof(DaxShmBlock));
            m_shmBlock = nullptr;
        }
    }

    DaxShmBlock* m_shmBlock{nullptr};
    std::chrono::steady_clock::time_point m_lastRetry{};
};

// ── AudioStreamBasicDescription helper ──────────────────────────────────────

static AudioStreamBasicDescription makePCMFormat(Float64 sampleRate, UInt32 channels)
{
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    fmt.mChannelsPerFrame = channels;
    fmt.mBytesPerFrame    = channels * sizeof(float);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    return fmt;
}

// ── DriverRequestHandler: defers device creation until host is ready ─────────

class DaxDriverHandler : public aspl::DriverRequestHandler {
public:
    DaxDriverHandler(std::shared_ptr<aspl::Context> ctx,
                     std::shared_ptr<aspl::Plugin> plug)
        : m_context(std::move(ctx))
        , m_plugin(std::move(plug))
    {}

    OSStatus OnInitialize() override
    {
        // Called by HAL after driver is fully initialized and host is available.
        // Safe to add devices here — PropertiesChanged notifications will work.

        // 4 DAX RX input devices (radio → apps receive audio)
        for (int ch = 1; ch <= 4; ++ch) {
            char name[64];
            snprintf(name, sizeof(name), "MasterSDR DAX %d", ch);

            char uid[64];
            snprintf(uid, sizeof(uid), "com.mastersdr.dax.rx.%d", ch);

            auto handler = std::make_shared<DaxRxHandler>(ch);

            aspl::DeviceParameters devParams;
            devParams.Name         = name;
            devParams.Manufacturer = "MasterSDR";
            devParams.DeviceUID    = uid;
            devParams.ModelUID     = "com.mastersdr.dax";
            devParams.SampleRate   = 24000;
            devParams.ChannelCount = 2;
            devParams.EnableMixing = true;

            auto device = std::make_shared<aspl::Device>(m_context, devParams);
            device->SetIOHandler(handler);

            aspl::StreamParameters streamParams;
            streamParams.Direction = aspl::Direction::Input;
            streamParams.Format    = makePCMFormat(24000, 2);

            device->AddStreamWithControlsAsync(streamParams);
            m_plugin->AddDevice(device);

            // Keep shared_ptrs alive
            m_handlers.push_back(handler);
            m_devices.push_back(device);
        }

        // 1 TX output device (apps send audio → radio)
        {
            auto txHandler = std::make_shared<DaxTxHandler>();

            aspl::DeviceParameters txParams;
            txParams.Name         = "MasterSDR TX";
            txParams.Manufacturer = "MasterSDR";
            txParams.DeviceUID    = "com.mastersdr.dax.tx";
            txParams.ModelUID     = "com.mastersdr.dax";
            txParams.SampleRate   = 24000;
            txParams.ChannelCount = 2;
            txParams.EnableMixing = true;

            auto txDevice = std::make_shared<aspl::Device>(m_context, txParams);
            txDevice->SetIOHandler(txHandler);

            aspl::StreamParameters txStreamParams;
            txStreamParams.Direction = aspl::Direction::Output;
            txStreamParams.Format    = makePCMFormat(24000, 2);

            txDevice->AddStreamWithControlsAsync(txStreamParams);
            m_plugin->AddDevice(txDevice);

            m_handlers.push_back(txHandler);
            m_devices.push_back(txDevice);
        }

        return kAudioHardwareNoError;
    }

private:
    std::shared_ptr<aspl::Context> m_context;
    std::shared_ptr<aspl::Plugin> m_plugin;
    std::vector<std::shared_ptr<aspl::IORequestHandler>> m_handlers;
    std::vector<std::shared_ptr<aspl::Device>> m_devices;
};

// ── Driver entry point ──────────────────────────────────────────────────────

extern "C" void* MasterSDRDAX_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    auto context = std::make_shared<aspl::Context>(
        std::make_shared<aspl::Tracer>());

    auto plugin = std::make_shared<aspl::Plugin>(context);

    // Devices are created in OnInitialize() after host is ready
    auto driverHandler = std::make_shared<DaxDriverHandler>(context, plugin);

    static auto driver = std::make_shared<aspl::Driver>(context, plugin);
    driver->SetDriverHandler(driverHandler);

    // Keep handler alive
    static auto handlerRef = driverHandler;

    return driver->GetReference();
}
