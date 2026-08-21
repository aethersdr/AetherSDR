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

// Express an arbitrary steady_clock instant on the same relative-ms axis as
// cwTraceNowMs(), so scheduled times and wall times in one log line are
// directly comparable.  Instants before the epoch clamp to 0 rather than
// wrapping the unsigned result.
//
// Reading the log: the epoch is pinned by whichever trace call runs first,
// which for a key-edge line is the cwTraceNowMs() beside it.  The first
// traced edge's scheduled instant therefore predates the epoch by the wake
// latency and prints as schedMs=0.  That 0 is the clamp, not a measured
// zero-latency wake — a parser summarising t − schedMs should drop the
// first edge rather than treat it as an outlier.
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
