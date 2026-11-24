// The two FIFOs between the sound card's threads and the station's (the
// receive thread reads capture, the transmit thread writes playback), with
// data2g/audio.py's Capture and Player semantics.
//
// Capture: a stall in the reader becomes latency, never lost samples (a
// blocking read once dropped ~50 s of a 14 min session, holes inside
// bursts). Player: the writer hands over 0.1 s at a time and the device
// pulls in small periods, with `lead` queued ahead at key-up so a late
// step does not leave the device empty.
//
// SSTVAE's rule holds: nothing holds a lock across a bulk copy. Both are a
// single-producer single-consumer ring whose only shared state is two
// atomic counters; the capture side's condition variable guards nothing
// but the reader's sleep. SSTVAE's rx::RingBuffer is not used: it
// overwrites the oldest samples, which here would be lost audio.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace data2g::audio {

// Wait-free single-producer single-consumer ring. write() only from one
// thread, read() only from one other.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity) : buf_(capacity) {}

    // Writes what fits; returns how many were written.
    std::size_t write(std::span<const T> x) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t n = std::min(x.size(), buf_.size() - static_cast<std::size_t>(head - tail));
        copy_in(head, x.first(n));
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Reads what is there, up to out.size(); returns how many were read.
    std::size_t read(std::span<T> out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::size_t n = std::min(out.size(), static_cast<std::size_t>(head - tail));
        const std::size_t cap = buf_.size(), pos = static_cast<std::size_t>(tail % cap);
        const std::size_t run = std::min(n, cap - pos);
        std::copy_n(buf_.data() + pos, run, out.data());
        std::copy_n(buf_.data(), n - run, out.data() + run);
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer only: throws away what is queued.
    void discard() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

    std::size_t size() const noexcept {
        return static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
    }
    std::size_t capacity() const noexcept { return buf_.size(); }

private:
    void copy_in(std::uint64_t head, std::span<const T> x) noexcept {
        const std::size_t cap = buf_.size(), pos = static_cast<std::size_t>(head % cap);
        const std::size_t run = std::min(x.size(), cap - pos);
        std::copy_n(x.data(), run, buf_.data() + pos);
        std::copy_n(x.data() + run, x.size() - run, buf_.data());
    }

    std::vector<T> buf_;
    std::atomic<std::uint64_t> head_{0}, tail_{0};  // samples ever written / read
};

// Capture: the device thread write()s mono samples (already at the
// engine's rate, see CapturePipeline), the receive (engine) thread read()s blocks.
class CaptureFifo {
public:
    // `rate`: of the samples written. Holds `capacity_s` of backlog; beyond
    // that new samples are dropped and counted (a reader stalled that long
    // is broken anyway).
    explicit CaptureFifo(int rate, double capacity_s = 60.0);

    // Device thread.
    void write(std::span<const double> x);
    // The device reported input lost before we saw it (PortAudio's
    // paInputOverflow; a backend that can tell calls this).
    void overflow() { overflows_.fetch_add(1, std::memory_order_relaxed); }

    // Engine thread: the next `n` samples, waiting for them; nothing once
    // close() has been called and fewer than `n` are left.
    std::optional<std::vector<double>> read(std::size_t n);
    // Unblocks read(), from any thread.
    void close();

    std::size_t backlog() const { return ring_.size(); }
    double backlog_s() const { return static_cast<double>(backlog()) / rate_; }
    std::uint64_t overflows() const { return overflows_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Times the backlog went over LATE_S (host.py's "RX audio behind" warning).
    std::uint64_t late_events() const { return late_events_.load(std::memory_order_relaxed); }
    static constexpr double LATE_S = 1.0;

private:
    int rate_;
    SpscRing<double> ring_;
    std::mutex m_;  // guards only the reader's sleep, never data
    std::condition_variable cv_;
    std::atomic<bool> closed_{false};
    bool late_ = false;  // reader thread only
    std::atomic<std::uint64_t> overflows_{0}, dropped_{0}, late_events_{0};
};

// Playback: the transmit thread (app::Transmitter, rig::Keyer) write()s
// mono samples at the device rate, the device thread pull()s periods.
class PlaybackFifo {
public:
    PlaybackFifo(int rate, double lead_s, double capacity_s = 30.0);

    // Writer's thread. Key-up: queue the lead, then count short pulls as
    // underruns.
    void start();
    // Waits for room rather than dropping TX audio.
    void write(std::span<const double> y);
    // Throws away what is queued but not yet pulled (an abort), done by the device thread on its next pull();
    // waits for that, up to 0.2 s (a stopped device never pulls).
    void flush();
    // Waits until everything queued has left the sound card (FIFO empty,
    // then the backend's output latency), so PTT can drop without clipping.
    // Gives up after 1 s with nothing pulled (a stopped device).
    void drain();

    // Device thread: fills `out` (mono), zero-padding a shortfall. Returns
    // the samples that were real audio.
    std::size_t pull(std::span<float> out);

    // Set by the backend: how long the device holds audio after pull().
    void set_output_latency(double s) { latency_s_.store(s, std::memory_order_relaxed); }
    double output_latency() const { return latency_s_.load(std::memory_order_relaxed); }

    std::size_t queued() const { return ring_.size(); }
    std::uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    int rate() const { return rate_; }

private:
    int rate_;
    std::size_t lead_;
    SpscRing<float> ring_;
    std::atomic<bool> active_{false}, flush_{false};
    std::atomic<double> latency_s_{0.0};
    std::atomic<std::uint64_t> underruns_{0};
    std::vector<float> scratch_;  // writer's thread only
};

}  // namespace data2g::audio
