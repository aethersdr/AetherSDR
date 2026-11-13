#include "rig/keyer.hpp"

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace data2g::rig {

Keyer::Keyer(Ptt ptt, audio::PlaybackFifo& out, double off_delay_s, Report on_error, MustRelease must_release)
    : ptt_(std::move(ptt)), out_(out), off_delay_s_(off_delay_s), report_(std::move(on_error)),
      must_release_(std::move(must_release)) {}

Keyer::~Keyer() {
    if (must_release_ ? must_release_() : sent_on_) ptt(false);
}

void Keyer::key() {
    if (keyed_) return;
    ptt(true);
    out_.start();
    keyed_ = true;
}

void Keyer::unkey(const std::function<void()>& drained) {
    if (!keyed_) return;
    if (off_failed_ == 0) {  // a retry has drained and waited already
        out_.drain();
        if (off_delay_s_ > 0) std::this_thread::sleep_for(std::chrono::duration<double>(off_delay_s_));
    }
    if (drained) drained();
    // A failed off leaves keyed_ set so the next call tries again, a few times: a rig stuck
    // transmitting is worse than one more try, but a dead one must not hold the loop forever.
    constexpr int MAX_OFF_TRIES = 5;
    if (ptt(false) || ++off_failed_ >= MAX_OFF_TRIES) {
        keyed_ = false;
        off_failed_ = 0;
    }
}

bool Keyer::ptt(bool on) {
    if (!ptt_) return true;
    if (on) sent_on_ = true;
    try {
        ptt_(on);
        return true;
    } catch (const std::exception& e) {
        ++failures_;
        if (report_) report_(std::string("PTT ") + (on ? "on" : "off") + " failed: " + e.what());
        return false;
    }
}

}  // namespace data2g::rig
