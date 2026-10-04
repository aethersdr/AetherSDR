#pragma once

#include <array>
#include <cstddef>

namespace AetherSDR {

// Non-atomic counters: every acquire, release and query MUST run on the main
// thread. Concurrent access is a data race. Never persisted. The vault callback
// owns the guard so destroying Setup cannot permit a reconnect mid-delete.
class PeripheralRemovalGuard {
public:
    enum class Device { Tgxl, Pgxl, AntennaGenius };

    explicit PeripheralRemovalGuard(Device device) : m_device(device)
    {
        ++s_pending[static_cast<std::size_t>(device)];
    }
    ~PeripheralRemovalGuard()
    {
        --s_pending[static_cast<std::size_t>(m_device)];
    }
    PeripheralRemovalGuard(const PeripheralRemovalGuard&) = delete;
    PeripheralRemovalGuard& operator=(const PeripheralRemovalGuard&) = delete;

    static bool pending(Device device)
    {
        return s_pending[static_cast<std::size_t>(device)] != 0;
    }

private:
    Device m_device;
    inline static std::array<unsigned, 3> s_pending{};
};

} // namespace AetherSDR
