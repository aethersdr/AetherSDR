#pragma once

#include <QtGlobal>

#include <atomic>
#include <chrono>

namespace AetherSDR {

inline std::chrono::steady_clock::time_point cwTraceEpoch() noexcept
{
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

// Express a steady_clock instant on cwTraceNowMs()'s relative-ms axis; instants
// before the epoch clamp to 0. The epoch is pinned by the first trace call, so
// the first edge's scheduled instant prints as schedMs=0 (the clamp, not a zero
// wake latency); drop it when summarising t - schedMs.
inline quint64 cwTraceMsAt(std::chrono::steady_clock::time_point tp) noexcept
{
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp - cwTraceEpoch()).count();
    return delta > 0 ? static_cast<quint64>(delta) : 0;
}

inline quint64 cwTraceNowMs() noexcept
{
    return cwTraceMsAt(std::chrono::steady_clock::now());
}

inline quint64 nextCwTraceId() noexcept
{
    static std::atomic<quint64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace AetherSDR
