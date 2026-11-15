#include "core/WaveformUploadState.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    bool ok = true;
    AetherSDR::WaveformUploadState state;

    const auto first = state.begin(10);
    ok &= expect(state.nextChunkSize(first, 6) == 6,
                 "first request is bounded by chunk size");
    ok &= expect(state.recordQueued(first, 6, 4),
                 "partial socket write is accepted");
    ok &= expect(state.queuedBytes() == 4,
                 "queue offset advances by accepted bytes");
    ok &= expect(state.acknowledge(first, 2),
                 "partial acknowledgement is accepted");
    ok &= expect(state.acknowledgedBytes() == 2 && state.queuedBytes() == 4,
                 "acknowledgement does not change the queue offset");
    ok &= expect(state.nextChunkSize(first, 64) == 6,
                 "next write begins after queued bytes, not acknowledged bytes");
    ok &= expect(state.recordQueued(first, 6, 6),
                 "remaining bytes queue without overlap");
    ok &= expect(state.acknowledge(first, 8) && state.complete(first),
                 "transfer completes only after every queued byte is acknowledged");

    const auto second = state.begin(20);
    ok &= expect(!state.recordQueued(first, 10, 10),
                 "callback from an old generation is rejected");
    ok &= expect(state.queuedBytes() == 0,
                 "stale callback cannot mutate the new operation");
    ok &= expect(!state.acknowledge(second, 1),
                 "socket cannot acknowledge bytes that were never queued");
    ok &= expect(state.recordQueued(second, 20, 20),
                 "new generation accepts its own write");

    state.invalidate();
    ok &= expect(!state.isCurrent(second)
                 && !state.acknowledge(second, 20),
                 "cancelled generation rejects delayed socket notifications");

    return ok ? 0 : 1;
}
