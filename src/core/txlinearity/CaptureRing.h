#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace AetherSDR::txlin {

// Lock-free single-producer / single-consumer ring for handing capture frames
// from the thread that receives IQ to the thread that transforms it.
//
// WHY LOCK-FREE. The producer side runs on the thread that services the radio's
// VITA-49 stream, which is the same thread that services the GUI. If it ever
// blocks on a mutex held by an analysis thread mid-FFT, the panadapter stutters
// and — far worse — the stream backs up while the radio is keyed. The one
// property that matters here is that push() cannot wait for pop(), and a
// bounded SPSC ring with acquire/release indices gives exactly that with no
// atomics contention beyond two cache lines.
//
// OVERFLOW IS DROP-NEWEST, DELIBERATELY. When the analysis thread falls behind,
// the ring refuses the incoming frame rather than overwriting the oldest. A
// linearity measurement Welch-averages whole frames and does not care WHICH
// frames it gets, only that each one is internally contiguous — so dropping is
// harmless, while overwriting a frame the consumer is mid-read on would splice
// two different capture instants together and manufacture spectral content that
// was never transmitted. Dropped frames are counted so the UI can report a
// measurement that did not get the averaging it asked for.
//
// Capacity is rounded up to a power of two so the index wrap is a mask.
template <typename T>
class CaptureRing {
public:
    explicit CaptureRing(std::size_t capacity)
    {
        std::size_t cap = 2;
        while (cap < capacity) {
            cap <<= 1;
        }
        m_slots.resize(cap);
        m_mask = cap - 1;
    }

    [[nodiscard]] std::size_t capacity() const { return m_slots.size(); }

    // Producer side. Returns false when the ring is full; the caller must treat
    // that as a dropped frame, not as a retry-later.
    bool push(T&& value)
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & m_mask;
        if (next == m_tail.load(std::memory_order_acquire)) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[head] = std::move(value);
        m_head.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out)
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        out = std::move(m_slots[tail]);
        m_tail.store((tail + 1) & m_mask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const
    {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t dropped() const
    {
        return m_dropped.load(std::memory_order_relaxed);
    }

    // Safe only when neither side is running — between runs.
    void reset()
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
        m_dropped.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<T> m_slots;
    std::size_t m_mask = 0;
    // Separate cache lines: head is written only by the producer and tail only
    // by the consumer, and sharing a line would put them in false contention on
    // every single frame.
    alignas(64) std::atomic<std::size_t> m_head{0};
    alignas(64) std::atomic<std::size_t> m_tail{0};
    alignas(64) std::atomic<std::size_t> m_dropped{0};
};

}  // namespace AetherSDR::txlin
