#include "util/pool.hpp"

#include "util/cancel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <utility>
#endif

namespace data2g::pool {
namespace {

thread_local bool in_task = false;

std::int64_t process_id() {
#ifdef _WIN32
    return 0;
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

// ponytail: one job at a time; a second caller runs inline. Per-caller
// queues if two threads ever both need the pool at once.
class Pool {
public:
    ~Pool() {
        if (owner_ == process_id()) resize(1);
    }

    int size() {
        std::lock_guard lk(job_);
        start();
        return size_;
    }

    void set(int n) {
        std::lock_guard lk(job_);
        start();
        resize(n < 1 ? default_threads() : n);
    }

    void run(std::size_t n, const std::function<void(std::size_t)>& fn) {
        std::unique_lock job(job_, std::try_to_lock);
        if (job) start();
        if (!job || size_ == 1 || n < 2 || in_task) {
            if (job) job.unlock();
            for (std::size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        {
            std::unique_lock lk(m_);
            idle_.wait(lk, [&] { return active_ == 0; });  // a late waker of the last job has left
            fn_ = &fn;
            cancel_ = cancel::current();
            n_ = n;
            next_.store(0, std::memory_order_relaxed);
            error_ = nullptr;
            ++gen_;
        }
        wake_.notify_all();
        in_task = true;
        work();
        in_task = false;
        std::unique_lock lk(m_);
        idle_.wait(lk, [&] { return active_ == 0; });
        fn_ = nullptr;
        if (error_) std::rethrow_exception(error_);
    }

private:
    // Claims indices until none are left.
    void work() {
        const cancel::Scope scope(cancel_);  // the caller's, whichever thread runs this
        for (std::size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < n_;) {
            try {
                (*fn_)(i);
            } catch (...) {
                std::lock_guard lk(m_);
                if (!error_) error_ = std::current_exception();
                next_.store(n_, std::memory_order_relaxed);
            }
        }
    }

    void worker() {
        in_task = true;
        std::uint64_t seen = 0;
        std::unique_lock lk(m_);
        for (;;) {
            wake_.wait(lk, [&] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
            ++active_;
            lk.unlock();
            work();
            lk.lock();
            if (--active_ == 0) idle_.notify_all();
        }
    }

    // Under job_: the default size on first use, and again in a fork's
    // child (Python studies fork), which has none of the parent's threads.
    void start() {
        if (owner_ != process_id()) {
            new std::vector<std::thread>(std::move(threads_));  // the parent's: neither joinable nor destructible here
            threads_.clear();
            size_ = 0;
            owner_ = process_id();
        }
        if (!size_) resize(default_threads());
    }

    // Under job_.
    void resize(int n) {
        if (n == size_) return;
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
        stop_ = false;
        size_ = n;
        for (int i = 1; i < n; ++i) threads_.emplace_back([this] { worker(); });
    }

    std::mutex job_;  // held by the thread running a job, and while resizing
    int size_ = 0;    // 0: not started
    std::int64_t owner_ = process_id();
    std::vector<std::thread> threads_;

    std::mutex m_;  // guards the job below and the wakeups
    std::condition_variable wake_, idle_;
    const std::function<void(std::size_t)>* fn_ = nullptr;
    const cancel::Expired* cancel_ = nullptr;  // the submitter's, valid until it returns
    std::size_t n_ = 0;
    std::atomic<std::size_t> next_{0};
    std::exception_ptr error_;
    std::uint64_t gen_ = 0;
    int active_ = 0;  // workers inside work()
    bool stop_ = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

}  // namespace

int default_threads() {
    if (const char* env = std::getenv("DATA2G_THREADS"); env && std::atoi(env) > 0) return std::atoi(env);
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency() / 2), 1, 4);
}

void set_threads(int n) { pool().set(n); }
int threads() { return pool().size(); }

void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) { pool().run(n, fn); }

}  // namespace data2g::pool
