// host.py serve()'s key-up / key-down sequence around the TX FIFO, on the
// station's transmit thread (app::Transmitter). The PTT on delay is not
// here: the engine puts that much silence ahead of a burst (Engine
// ptt_delay_s, --ptt-on-delay-ms).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "audio/fifo.hpp"

namespace data2g::rig {

class Keyer {
public:
    // RigController::ptt_function(), or empty for no PTT (--rigctld-port 0).
    using Ptt = std::function<void(bool)>;
    using Report = std::function<void(const std::string&)>;

    // Whether the exit path must send PTT off: RigController::keyed_since_open.
    using MustRelease = std::function<bool()>;

    // `off_delay_s`: --ptt-off-delay-ms, between the audio draining and PTT off.
    // `must_release` empty: whether this Keyer ever sent PTT on.
    Keyer(Ptt ptt, audio::PlaybackFifo& out, double off_delay_s, Report on_error = {}, MustRelease must_release = {});
    // PTT off if `must_release` says the rig was keyed; otherwise nothing
    // (no call, no wait, no warning). unkey() always sends it.
    ~Keyer();

    Keyer(const Keyer&) = delete;
    Keyer& operator=(const Keyer&) = delete;

    void key();    // PTT on, then the TX lead queued
    // Drain the FIFO and the device, wait off_delay_s, call `drained`, PTT off. `drained` comes before the
    // PTT off on purpose: the CAT round trip can take longer than the peer's turnaround, and receiving
    // must not wait for it (recordings/20261011-223759: acks lost to a mute held through it).
    void unkey(const std::function<void()>& drained = {});
    bool keyed() const { return keyed_.load(std::memory_order_acquire); }
    // PTT calls that failed (logged and carried on from, as tnc.Rigctld does).
    std::uint64_t failures() const { return failures_; }

private:
    bool ptt(bool on);  // false: the rig did not take it

    Ptt ptt_;
    audio::PlaybackFifo& out_;
    double off_delay_s_;
    Report report_;
    MustRelease must_release_;
    std::atomic<bool> keyed_{false};  // the transmit thread keys and unkeys
    bool sent_on_ = false;
    std::uint64_t failures_ = 0;
    int off_failed_ = 0;  // consecutive failed PTT offs, retried on the next unkey() up to MAX_OFF_TRIES
};

}  // namespace data2g::rig
