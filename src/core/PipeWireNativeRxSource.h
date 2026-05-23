#pragma once

#include <spa/utils/hook.h>

#include <atomic>
#include <cstdint>

struct pw_stream;

namespace MasterSDR {

// One PipeWire native virtual source (Audio/Source) per DAX RX channel.
// Replaces module-pipe-source on Linux when libpipewire-0.3 dev headers are
// present at build time.  Avoids both the kernel FIFO and the pulse-compat
// translation, and lets us properly request a small node quantum via
// PW_KEY_NODE_LATENCY so PipeWire's graph negotiates lower client buffers.
//
// Threading:
//   - feedAudio() is called from the Qt main thread (radio audio path).
//   - on_process() runs on PipeWire's real-time loop thread.
//   - The two communicate via a fixed-size SPSC float ring buffer with
//     std::atomic head/tail indices — no locks, no allocations on the
//     real-time path.
class PipeWireNativeRxSource {
public:
    explicit PipeWireNativeRxSource(int channel);
    ~PipeWireNativeRxSource();

    PipeWireNativeRxSource(const PipeWireNativeRxSource&) = delete;
    PipeWireNativeRxSource& operator=(const PipeWireNativeRxSource&) = delete;

    // Acquires the shared PipeWireNativeContext, creates the pw_stream, and
    // connects it as a virtual Audio/Source node at 48 kHz mono float32.
    // Returns true on success.  Safe to call only once per instance.
    bool open();

    // Disconnects and destroys the stream, releases the shared context.
    void close();

    // Push 48 kHz mono float32 samples into the ring buffer.  Drops the
    // newest incoming samples that wouldn't fit if the consumer (PipeWire)
    // is too slow — this caps backlog at the ring size (~42 ms) so latency
    // cannot grow unboundedly even under stalls, and it keeps the SPSC
    // invariant intact (only the producer touches m_writeIdx, only the
    // consumer touches m_readIdx).
    void feedAudio(const float* samples, uint32_t count);

    // PipeWire C callbacks — public so the kStreamEvents POD in the .cpp can
    // take their address.  Not part of the user API.
    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, int old, int state, const char* error);

private:

    // Ring buffer is a power of two so wraparound is a mask op.
    // 2048 samples = ~42 ms @ 48 kHz mono float32.  In steady state the ring
    // is near-empty; the cap matters only on transient stalls — at which
    // point we *want* to drop oldest samples rather than let latency grow.
    // Earlier 8192 sizing absorbed up to 170 ms of stalls, which directly
    // showed up as DT growth under load.
    static constexpr uint32_t RING_SIZE = 2048;
    static constexpr uint32_t RING_MASK = RING_SIZE - 1;

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
    float                  m_ring[RING_SIZE]{};
    std::atomic<uint32_t>  m_writeIdx{0};
    std::atomic<uint32_t>  m_readIdx{0};
};

} // namespace MasterSDR
