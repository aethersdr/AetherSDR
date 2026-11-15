// Socket-free check of AudioEngine::joinThreadWhilePlannerBusy (#6287): the
// shutdown join keeps waiting while another thread holds an FFTW planner lock,
// returns at once for a thread that has finished, and still gives up on a
// thread that is stuck on something other than the planner.
#include "core/AudioEngine.h"
#include "core/dsp/FftwPlannerLock.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using AetherSDR::AudioEngine;

// A planner holder on another thread keeps the lock for holdMs, while the
// joined thread has to take the same lock before it can finish.
bool waitsOutHolder(const char* name, std::mutex& planner, int holdMs)
{
    std::atomic<bool> holding {false};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock(planner);
        holding.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    });
    while (!holding.load()) {
        std::this_thread::yield();
    }
    std::unique_ptr<QThread> joined(QThread::create([&planner] {
        std::lock_guard<std::mutex> lock(planner);
    }));
    joined->start();

    QElapsedTimer timer;
    timer.start();
    const bool ok = AudioEngine::joinThreadWhilePlannerBusy(*joined, 1000, "test.join");
    const qint64 ms = timer.elapsed();
    holder.join();
    joined->wait();
    if (!ok || ms < holdMs - 500) {
        std::cerr << "FAIL: " << name << " (ok=" << ok << ", " << ms << " ms)\n";
        return false;
    }
    std::cout << "PASS: " << name << " (" << ms << " ms)\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    ok &= waitsOutHolder("waits while the double-precision planner is held",
                         AetherSDR::fftwPlannerMutex(), 4000);
    ok &= waitsOutHolder("waits while the single-precision planner is held",
                         AetherSDR::fftwfPlannerMutex(), 4000);

    {
        std::unique_ptr<QThread> quick(QThread::create([] {}));
        quick->start();
        QElapsedTimer timer;
        timer.start();
        const bool joined = AudioEngine::joinThreadWhilePlannerBusy(*quick, 3000, "test.join");
        const qint64 ms = timer.elapsed();
        if (!joined || ms > 500) {
            std::cerr << "FAIL: finished thread joins at once (ok=" << joined
                      << ", " << ms << " ms)\n";
            ok = false;
        } else {
            std::cout << "PASS: finished thread joins at once (" << ms << " ms)\n";
        }
    }

    {
        std::unique_ptr<QThread> stuck(QThread::create([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
        }));
        stuck->start();
        QElapsedTimer timer;
        timer.start();
        const bool joined = AudioEngine::joinThreadWhilePlannerBusy(*stuck, 1000, "test.join");
        const qint64 ms = timer.elapsed();
        stuck->wait();
        if (joined || ms > 2500) {
            std::cerr << "FAIL: gives up when no planner lock is held (ok=" << joined
                      << ", " << ms << " ms)\n";
            ok = false;
        } else {
            std::cout << "PASS: gives up when no planner lock is held (" << ms << " ms)\n";
        }
    }

    return ok ? 0 : 1;
}
