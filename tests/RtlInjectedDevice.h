#pragma once
#include "core/backends/rtl/RtlSdrWorker.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace AetherSDR::test {
using T = rtl::RtlCaptureTransaction;
struct DeviceState {
    std::mutex mutex;
    std::condition_variable changed;
    bool canceled = false;
    bool holdReadback = true;
    bool inReadback = false;
    int readbackPermits = 0;
    int blocks = 0;
    std::atomic<int> starts{0}, cancels{0}, writes{0}, callbacks{0};
    std::atomic<bool> badReadback{false};
    std::atomic<std::uint32_t> callbackBytes{16384};
    std::atomic<unsigned char> iqLevel{130};
    std::atomic<int> failWriteAt{0};
    std::atomic<bool> destroyed{false};
    T::Hardware hardware;
    void releaseReadback()
    {
        std::lock_guard lock(mutex); holdReadback = false; changed.notify_all();
    }
    void releaseOneReadback()
    {
        std::lock_guard lock(mutex); ++readbackPermits; changed.notify_all();
    }
    void block()
    {
        std::lock_guard lock(mutex); ++blocks; changed.notify_all();
    }
};
class InjectedDevice final : public rtl::RtlSdrWorker::Device {
public:
    explicit InjectedDevice(std::shared_ptr<DeviceState> state) : m_state(std::move(state)) {}
    ~InjectedDevice() override { m_state->destroyed = true; }
    bool set(T::Control control, std::int64_t value) override
    {
        const int write = ++m_state->writes;
        if (write == m_state->failWriteAt.load()) { return false; }
        switch (control) {
        case T::Control::DirectSampling: m_state->hardware.directSampling = int(value); break;
        case T::Control::SampleRate: m_state->hardware.sampleRateHz = std::uint32_t(value); break;
        case T::Control::Ppm: m_state->hardware.ppm = int(value); break;
        case T::Control::OffsetTuning: m_state->hardware.offsetTuning = int(value); break;
        case T::Control::Center: m_state->hardware.centerHz = std::uint32_t(value); break;
        case T::Control::Gain: m_state->hardware.gainTenths = int(value); break;
        }
        return true;
    }
    std::optional<T::Hardware> read() override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->inReadback = true;
        m_state->changed.notify_all();
        m_state->changed.wait(lock, [&] {
            return !m_state->holdReadback || m_state->readbackPermits > 0;
        });
        if (m_state->readbackPermits > 0) { --m_state->readbackPermits; }
        m_state->inReadback = false;
        auto actual = m_state->hardware;
        if (m_state->badReadback) { actual.sampleRateHz = 0; }
        return actual;
    }
    bool resetBuffer() override { return true; }
    int readAsync(Callback callback, void* context) override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->canceled = false;
        ++m_state->starts;
        std::array<unsigned char, 16384> iq;
        iq.fill(130);
        while (!m_state->canceled) {
            m_state->changed.wait(lock, [&] { return m_state->canceled || m_state->blocks > 0; });
            if (m_state->canceled) { break; }
            --m_state->blocks;
            lock.unlock();
            iq.fill(m_state->iqLevel.load());
            callback(iq.data(), m_state->callbackBytes.load(), context);
            ++m_state->callbacks;
            lock.lock();
        }
        return 0;
    }
    void cancelAsync() override
    {
        ++m_state->cancels;
        std::lock_guard lock(m_state->mutex);
        m_state->canceled = true;
        m_state->changed.notify_all();
    }
private:
    std::shared_ptr<DeviceState> m_state;
};
} // namespace AetherSDR::test
