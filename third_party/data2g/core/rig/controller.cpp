#include "rig/controller.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace data2g::rig {

namespace {

using Clock = std::chrono::steady_clock;

std::chrono::nanoseconds secs(double s) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(s));
}

std::string first_line(const std::string& s) {
    const std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string mhz_text(double hz) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "Rig: %.4f MHz", hz / 1e6);
    return buf;
}

}  // namespace

double next_interval(double current, double base, bool ok) {
    return ok ? base : std::min(current * 2.0, MAX_BACKOFF_S);
}

// Everything the worker touches, co-owned by the worker and the
// controller. See the header: this is how "stop() detaches and the
// worker's exit path owns closing the handle" is expressed.
struct RigSession {
    std::unique_ptr<RigBackend> backend;
    RigConfig config;
    OnFrequency on_frequency;
    OnStatus on_status;

    std::mutex m;
    std::condition_variable cv;
    bool stopping = false;
    bool paused = false;
    // For the exit path (keyed_since_open): open() succeeded, and a PTT on
    // was attempted on the open rig since.
    bool opened = false;
    bool dead = false;  // open() failed and the worker has gone: nobody will service a PTT job
    bool keyed = false;

    // A keying request waiting to be run, and its result. Only ever one:
    // the rig is either keyed or not, so a second request supersedes the
    // first rather than queueing behind it.
    struct PttJob {
        bool on;
        bool done = false;
        bool failed = false;
        std::string error;
    };
    std::shared_ptr<PttJob> ptt;

    std::optional<double> frequency;
    std::string last_status;

    ~RigSession() {
        // The worker is normally last to let go, so this runs on the
        // worker thread as it exits -- which is exactly the requirement:
        // a detached, possibly-wedged thread must close its own handle
        // without help.
        if (backend) backend->close();
        closed->store(true, std::memory_order_release);
    }

    std::shared_ptr<std::atomic<bool>> closed = std::make_shared<std::atomic<bool>>(false);

    void publish_status(const std::string& text, bool error) {
        // Locked because `stopping` and `last_status` are shared with
        // stop(); the callback itself runs unlocked so a slow UI cannot
        // stall the worker.
        OnStatus fn;
        {
            std::lock_guard<std::mutex> lock(m);
            if (stopping || text == last_status) return;
            last_status = text;
            fn = on_status;
        }
        if (fn) fn(text, error);
    }

    void publish_frequency(std::optional<double> hz) {
        OnFrequency fn;
        {
            std::lock_guard<std::mutex> lock(m);
            if (stopping) return;  // superseded; must not overwrite the new worker
            frequency = hz;
            fn = on_frequency;
        }
        if (fn) fn(hz);
    }
};

namespace {

// The worker. Takes the session by value so it co-owns it: when this
// returns, the last reference usually dies here and ~Session closes the
// backend.
void run(std::shared_ptr<RigSession> s) {
    try {
        s->backend->open();
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->opened = true;
        }
        s->publish_status("Rig: " + s->backend->description(), false);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->dead = true;
        }
        s->cv.notify_all();
        s->publish_status(first_line(e.what()), true);
        return;
    }

    const bool polling = s->config.poll_interval_s > 0;
    const double base = std::max(1.0, s->config.poll_interval_s);
    double interval = base;
    Clock::time_point next_poll = Clock::now();

    while (true) {
        std::shared_ptr<RigSession::PttJob> job;
        {
            std::unique_lock<std::mutex> lock(s->m);
            // Wake for: a stop, a keying request, or the poll deadline.
            // Paused means transmit is in progress, so there is no poll
            // deadline at all -- only a job, a stop, or resume_polling()
            // clearing `paused` should wake us there. That last one is
            // its own predicate rather than folded into the general
            // `wake` below: sharing one predicate between both branches
            // would make `!s->paused` true from the first instant of the
            // *unpaused* wait too, firing wait_until immediately on every
            // poll instead of at next_poll. As written, only the paused
            // wait watches `paused` -- and it has to, because resume_polling()
            // only clears the flag and notifies; a predicate that did not
            // check it would sleep straight through that notify and leave
            // polling dead until the next over keys the rig, which is what
            // issue #40 saw as "the frequency stops updating".
            const auto wake = [&] { return s->stopping || s->ptt != nullptr; };
            if (s->paused || !polling) {
                s->cv.wait(lock, [&] { return wake() || (polling && !s->paused); });
            } else {
                s->cv.wait_until(lock, next_poll, wake);
            }
            if (s->stopping) return;
            job = s->ptt;
            s->ptt.reset();
        }

        if (job) {
            // Priority work. Any poll that was due is simply skipped --
            // it would have been stale by the time it ran.
            if (job->on) {
                std::lock_guard<std::mutex> lock(s->m);
                s->keyed = true;  // attempted: a failed or timed-out on may still have keyed it
            }
            try {
                s->backend->set_ptt(job->on);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(s->m);
                job->failed = true;
                job->error = first_line(e.what());
            }
            {
                std::lock_guard<std::mutex> lock(s->m);
                job->done = true;
            }
            s->cv.notify_all();
            // Keying costs a poll interval of quiet: a rig that has just
            // been keyed is the last thing worth interrogating.
            next_poll = Clock::now() + secs(interval);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(s->m);
            if (s->stopping) return;
            if (s->paused) continue;  // raced with pause; no poll while keyed
        }
        if (!polling || Clock::now() < next_poll) continue;  // spurious wakeup

        bool ok = true;
        std::optional<double> value;
        std::string status;
        try {
            const double hz = s->backend->frequency_hz();
            value = hz;
            status = mhz_text(hz);
        } catch (const std::exception& e) {
            ok = false;
            status = first_line(e.what());
        }

        // Checked *after* the call: stop() may have arrived while it was
        // in flight, and a superseded worker must not publish.
        {
            std::lock_guard<std::mutex> lock(s->m);
            if (s->stopping) return;
        }
        s->publish_frequency(value);
        s->publish_status(status, !ok);

        interval = next_interval(interval, base, ok);
        next_poll = Clock::now() + secs(interval);
    }
}

}  // namespace

RigController::RigController(OnFrequency on_frequency, OnStatus on_status)
    : on_frequency_(std::move(on_frequency)), on_status_(std::move(on_status)) {}

RigController::~RigController() { stop(); }

void RigController::start(std::unique_ptr<RigBackend> backend,
                          const RigConfig& config) {
    stop();
    if (!backend) return;

    auto s = std::make_shared<RigSession>();
    s->backend = std::move(backend);
    s->config = config;
    s->on_frequency = on_frequency_;
    s->on_status = on_status_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = s;
    }
    if (on_status_) on_status_("Rig: connecting...", false);
    // Detached from birth. There is no point at which joining this
    // thread would be safe to do from a UI thread, so the ability is
    // not offered.
    std::thread(run, s).detach();
}

void RigController::stop() {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.swap(session_);
        if (s) departing_closed_ = s->closed;
    }
    if (!s) return;
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->stopping = true;
        s->paused = false;  // so a paused worker can notice and leave
        s->frequency.reset();
    }
    s->cv.notify_all();
    // And that is all. `s` goes out of scope here; if the worker is
    // mid-call it holds the last reference and cleans up when it
    // unwinds. Waiting for it is precisely what this must not do.
}

bool RigController::wait_for_shutdown(double seconds) {
    std::shared_ptr<std::atomic<bool>> closed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed = departing_closed_;
    }
    if (!closed) return true;
    const Clock::time_point deadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(seconds));
    while (!closed->load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool RigController::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ != nullptr;
}

std::optional<double> RigController::frequency_hz() const {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) return std::nullopt;
    std::lock_guard<std::mutex> lock(s->m);
    return s->frequency;
}

void RigController::set_ptt(bool on) {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) throw RigError("rig control is not running");

    auto job = std::make_shared<RigSession::PttJob>();
    job->on = on;
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->stopping) throw RigError("rig control is shutting down");
        if (s->dead) throw RigError("rig is not open");
        // Replaces any pending keying request rather than queueing: the
        // rig is either keyed or not. Pending *polls* are not a queue at
        // all here -- the worker only ever holds one poll deadline -- so
        // "discard pending polls" falls out of the design instead of
        // needing code.
        s->ptt = job;
    }
    s->cv.notify_all();

    std::unique_lock<std::mutex> lock(s->m);
    const bool finished = s->cv.wait_for(lock, secs(s->config.operation_timeout_s),
                                         [&] { return job->done || s->stopping || s->dead; });
    if (!finished || !job->done) {
        // The backstop for a backend whose own timeout did not fire.
        // Reported rather than waited out, because the caller is the
        // transmit sequence and it has a watchdog of its own.
        throw RigError("rig did not respond to PTT within " +
                       std::to_string(s->config.operation_timeout_s) + " s");
    }
    if (job->failed) throw RigError(job->error);
}

bool RigController::keyed_since_open() const {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->m);
    return s->opened && s->keyed;
}

std::function<void(bool)> RigController::ptt_function() {
    return [this](bool on) {
        if (on) {
            // A failed key leaves polling paused on purpose: a timed-out key may
            // still have keyed the rig, so no poll until the unkey, which the
            // Keyer always sends (it counts the over as keyed either way).
            pause_polling();
            set_ptt(true);
            return;
        }
        // resumed after the off, so no poll delays it; and resumed even if the off failed
        try {
            set_ptt(false);
        } catch (...) {
            resume_polling();
            throw;
        }
        resume_polling();
    };
}

void RigController::pause_polling() {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) return;
    std::lock_guard<std::mutex> lock(s->m);
    s->paused = true;
}

void RigController::resume_polling() {
    std::shared_ptr<RigSession> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) return;
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->paused = false;
    }
    s->cv.notify_all();
}

}  // namespace data2g::rig
