#pragma once

#include <QtGlobal>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

// Test-only real CPU work, measured independently of the production sampler.
// Workers park at both snapshot boundaries, so scheduling latency is never
// mistaken for CPU time and their lifetime can be controlled separately.
class MeasuredCpuWorkers {
public:
    static constexpr quint64 kTargetCpuUsecs = 50000;

    explicit MeasuredCpuWorkers(int count)
    {
        for (int i = 0; i < count; ++i) {
            m_threads.emplace_back([this]() {
                std::unique_lock lock(m_mutex);
                ++m_ready;
                m_changed.notify_all();
                m_changed.wait(lock, [this]() { return m_run || m_stop; });
                if (m_stop) {
                    return;
                }
                lock.unlock();
                const std::optional<quint64> start = threadCpuUsecs();
                std::optional<quint64> end = start;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                volatile quint64 sink = 0;
                while (start && end && *end >= *start && *end - *start < kTargetCpuUsecs
                       && std::chrono::steady_clock::now() < deadline) {
                    for (int j = 0; j < 4096; ++j) {
                        sink = sink * 6364136223846793005ULL + 1442695040888963407ULL;
                    }
                    end = threadCpuUsecs();
                }
                lock.lock();
                const bool valid = start && end && *end >= *start;
                const quint64 consumed = valid ? *end - *start : 0;
                m_cpuUsecs += consumed;
                m_complete = m_complete && valid && consumed >= kTargetCpuUsecs;
                ++m_done;
                m_changed.notify_all();
                m_changed.wait(lock, [this]() { return m_stop; });
            });
        }
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this]() { return m_ready == m_threads.size(); });
    }

    ~MeasuredCpuWorkers() { stop(); }

    bool run()
    {
        std::unique_lock lock(m_mutex);
        m_run = true;
        m_changed.notify_all();
        m_changed.wait(lock, [this]() { return m_done == m_threads.size(); });
        return m_complete;
    }

    quint64 cpuUsecs() const { return m_cpuUsecs; } // read after run() joins the work phase

    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
            m_changed.notify_all();
        }
        for (std::thread& thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    static std::optional<quint64> threadCpuUsecs()
    {
#ifdef Q_OS_WIN
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
            return std::nullopt;
        }
        const quint64 kernelTicks = (quint64(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        const quint64 userTicks = (quint64(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        return (kernelTicks + userTicks) / 10;
#else
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
            return std::nullopt;
        }
        return quint64(value.tv_sec) * 1000000 + quint64(value.tv_nsec) / 1000;
#endif
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::thread> m_threads;
    std::size_t m_ready{0};
    std::size_t m_done{0};
    quint64 m_cpuUsecs{0};
    bool m_run{false};
    bool m_stop{false};
    bool m_complete{true};
};
