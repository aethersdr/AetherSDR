#include "rig/trace.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace data2g::rig {
namespace {

std::mutex& trace_mu() {
    static std::mutex m;
    return m;
}

TraceSink& trace_sink() {
    static TraceSink sink;
    return sink;
}

// Read on the hot path, so it is not behind the mutex. Relaxed is
// enough: the worst a stale read can do is drop or add one line around
// the moment tracing is switched, and the sink itself is taken under
// the lock either way.
std::atomic<bool> g_tracing{false};

char nibble(std::uint8_t v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

}  // namespace

void set_trace_sink(TraceSink sink) {
    const bool active = static_cast<bool>(sink);
    {
        std::lock_guard<std::mutex> lock(trace_mu());
        trace_sink() = std::move(sink);
    }
    g_tracing.store(active, std::memory_order_relaxed);
}

bool tracing() { return g_tracing.load(std::memory_order_relaxed); }

void trace(const std::string& line) {
    if (!tracing()) return;
    TraceSink sink;
    {
        std::lock_guard<std::mutex> lock(trace_mu());
        sink = trace_sink();
    }
    // Outside the lock, for the reason `hamlib.cpp` records: the sink
    // is the app's and may take a lock of its own, and holding ours
    // across it would make the order between the two somebody else's
    // problem to get right.
    if (sink) sink(line);
}

std::string hex_bytes(const std::uint8_t* data, std::size_t n, std::size_t max) {
    std::string out;
    const std::size_t shown = n < max ? n : max;
    out.reserve(shown * 3 + 16);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) out += ' ';
        out += nibble(static_cast<std::uint8_t>(data[i] >> 4));
        out += nibble(static_cast<std::uint8_t>(data[i] & 0x0f));
    }
    if (shown < n) {
        out += " ... (";
        out += std::to_string(n);
        out += " bytes)";
    }
    return out;
}

}  // namespace data2g::rig
