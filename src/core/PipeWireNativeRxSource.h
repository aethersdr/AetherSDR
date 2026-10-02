#pragma once

#include <spa/utils/hook.h>

#include <atomic>
#include <cstdint>

struct pw_stream;

namespace AetherSDR {

// One PipeWire native Audio/Source per DAX RX channel (Linux, libpipewire-0.3).
// Requests a small quantum via PW_KEY_NODE_LATENCY. feedAudio() runs on the Qt
// main thread and on_process() on PipeWire's RT loop; they share a lock-free,
// allocation-free SPSC float ring with atomic head/tail.
class PipeWireNativeRxSource {
public:
    explicit PipeWireNativeRxSource(int channel, bool nativePcm = false);
    ~PipeWireNativeRxSource();

    PipeWireNativeRxSource(const PipeWireNativeRxSource&) = delete;
    PipeWireNativeRxSource& operator=(const PipeWireNativeRxSource&) = delete;

    // Acquires the shared PipeWireNativeContext, creates the pw_stream, and
    // connects it as a virtual Audio/Source node at 48 kHz mono float32.
    // Returns true on success.  Safe to call only once per instance.
    bool open();

    // Disconnects and destroys the stream, releases the shared context.
    void close();
    // Owner-thread boundary: discard retained samples under the existing loop lock.
    void reset();

    // Push 48 kHz mono float32 samples into the ring buffer.  Drops the
    // newest incoming samples that wouldn't fit if the consumer (PipeWire)
    // is too slow — this caps backlog at the configured ring size (42 or 341 ms) so latency
    // cannot grow unboundedly even under stalls, and it keeps the SPSC
    // invariant intact (only the producer touches m_writeIdx, only the
    // consumer touches m_readIdx).
    void feedAudio(const float* samples, uint32_t count);

    // PipeWire C callbacks — public so the kStreamEvents POD in the .cpp can
    // take their address.  Not part of the user API.
    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, int old, int state, const char* error);

private:

    // Legacy packet stream: 2048 samples (~42 ms). Native decoded bursts:
    // 16384 samples (~341 ms), matching the bounded RX-only FIFO path.
    static constexpr uint32_t kMaximumRingSize = 16384;
    const uint32_t m_ringSize;
    const uint32_t m_ringMask;

    int        m_channel;
    pw_stream* m_stream{nullptr};
    bool       m_contextAcquired{false};

    // PipeWire stream-event listener.  Owned by-value so the underlying
    // spa_hook lives exactly as long as this RxSource — no raw new/delete
    // and no leak across re-open() calls.
    spa_hook m_listenerHook{};

    // SPSC ring.  Producer (Qt thread) writes m_writeIdx; consumer
    // (PipeWire thread) writes m_readIdx.  Indices are free-running and
    // masked at access time.
    float                  m_ring[kMaximumRingSize]{};
    std::atomic<uint32_t>  m_writeIdx{0};
    std::atomic<uint32_t>  m_readIdx{0};
};

} // namespace AetherSDR
