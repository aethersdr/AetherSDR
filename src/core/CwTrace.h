#pragma once

#include <QtGlobal>

#include <atomic>
#include <chrono>

namespace MasterSDR {

inline quint64 cwTraceNowMs() noexcept
{
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count());
}

inline quint64 nextCwTraceId() noexcept
{
    static std::atomic<quint64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace MasterSDR
