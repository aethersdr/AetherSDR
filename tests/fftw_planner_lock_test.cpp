#include "core/NnrFilter.h"
#include "core/dsp/FftwPlannerLock.h"
#include "core/dsp/WdspChannel.h"
#ifdef HAVE_FFTW3
#include "core/SpectralNR.h"
#endif
#ifdef HAVE_SPECBLEACH
#include "core/SpecbleachFilter.h"
#endif
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrDdc.h"
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

template <typename Prepare, typename Operation, typename AcquireLock>
bool requiresLock(const char* name, Prepare prepare, Operation operation,
                  AcquireLock acquireLock)
{
    using namespace std::chrono_literals;

    // Warm the same operation without contention first. A slow constructor
    // must not masquerade as proof that it waited for the planner mutex.
    auto control = prepare();
    const auto controlStart = std::chrono::steady_clock::now();
    operation(control);
    const auto controlDuration = std::chrono::steady_clock::now() - controlStart;
    control.reset();
    if (controlDuration > 500ms) {
        std::cerr << "FAIL: " << name << " positive control was too slow\n";
        return false;
    }

    auto object = prepare();
    std::unique_lock<std::mutex> held = acquireLock();
    std::atomic<bool> started {false};
    std::atomic<bool> completed {false};
    std::thread worker([&] {
        started.store(true);
        operation(object);
        completed.store(true);
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(750ms);
    const bool blocked = !completed.load();
    held.unlock();
    worker.join();
    object.reset();
    if (!blocked || !completed.load()) {
        std::cerr << "FAIL: " << name << " did not wait for the planner lock\n";
        return false;
    }
    std::cout << "PASS: " << name << '\n';
    return true;
}

} // namespace

int main()
{
    bool ok = true;
#ifdef HAVE_FFTW3
    {
        AetherSDR::SpectralNR warm(256, 24000, 2);
    }
    {
        auto held = WdspChannel::fftwSetupLock();
        ok &= held.mutex() == &AetherSDR::fftwPlannerMutex();
    }
    ok &= requiresLock(
        "SpectralNR construction shares WDSP's lock",
        [] { return std::unique_ptr<AetherSDR::SpectralNR> {}; },
        [](std::unique_ptr<AetherSDR::SpectralNR>& nr) {
            nr = std::make_unique<AetherSDR::SpectralNR>(256, 24000, 2);
        },
        [] { return WdspChannel::fftwSetupLock(); });
    ok &= requiresLock(
        "SpectralNR destruction shares WDSP's lock",
        [] { return std::make_unique<AetherSDR::SpectralNR>(256, 24000, 2); },
        [](std::unique_ptr<AetherSDR::SpectralNR>& nr) { nr.reset(); },
        [] { return WdspChannel::fftwSetupLock(); });
    ok &= requiresLock(
        "NR2 wisdom import shares WDSP's lock",
        [] { return std::make_unique<int>(0); },
        [](std::unique_ptr<int>&) {
            static_cast<void>(AetherSDR::SpectralNR::loadWisdom(
                "aethersdr-fftw-planner-lock-test-absent"));
        },
        [] { return WdspChannel::fftwSetupLock(); });
#endif

    // NNR plans FFTW_PATIENT transforms inside WDSP's create_nnr(), outside
    // any WdspChannel, so its wrapper takes the lock itself (#6287). Built
    // unconditionally, like the WDSP it wraps. The warm-up puts its plans in
    // FFTW's in-process wisdom so the control below times the construction,
    // not a cold FFTW_PATIENT search.
    {
        AetherSDR::NnrFilter warm(48000);
        ok &= warm.isValid();
    }
    ok &= requiresLock(
        "NNR construction takes the double-precision lock",
        [] { return std::unique_ptr<AetherSDR::NnrFilter> {}; },
        [](std::unique_ptr<AetherSDR::NnrFilter>& nnr) {
            nnr = std::make_unique<AetherSDR::NnrFilter>(48000);
        },
        [] { return AetherSDR::fftwPlannerLock(); });
    ok &= requiresLock(
        "NNR destruction takes the double-precision lock",
        [] { return std::make_unique<AetherSDR::NnrFilter>(48000); },
        [](std::unique_ptr<AetherSDR::NnrFilter>& nnr) { nnr.reset(); },
        [] { return AetherSDR::fftwPlannerLock(); });
    {
        // An unsupported rate creates nothing, so its destructor has nothing
        // to destroy and must not wait on another thread's plan.
        auto held = AetherSDR::fftwPlannerLock();
        std::atomic<bool> completed {false};
        std::thread worker([&] {
            { AetherSDR::NnrFilter unsupported(44100); }
            completed.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool unblocked = completed.load();
        held.unlock();
        worker.join();
        if (unblocked) {
            std::cout << "PASS: NNR with nothing to destroy skips the lock\n";
        } else {
            std::cerr << "FAIL: NNR with nothing to destroy waited for the lock\n";
            ok = false;
        }
    }

#ifdef HAVE_SPECBLEACH
    {
        AetherSDR::SpecbleachFilter warm;
        ok &= warm.isValid();
    }
    ok &= requiresLock(
        "NR4 construction takes the single-precision lock",
        [] { return std::unique_ptr<AetherSDR::SpecbleachFilter> {}; },
        [](std::unique_ptr<AetherSDR::SpecbleachFilter>& nr) {
            nr = std::make_unique<AetherSDR::SpecbleachFilter>();
        },
        [] { return AetherSDR::fftwfPlannerLock(); });
    ok &= requiresLock(
        "NR4 destruction takes the single-precision lock",
        [] { return std::make_unique<AetherSDR::SpecbleachFilter>(); },
        [](std::unique_ptr<AetherSDR::SpecbleachFilter>& nr) { nr.reset(); },
        [] { return AetherSDR::fftwfPlannerLock(); });
#endif

#ifdef AETHER_BACKEND_RTL
    {
        AetherSDR::rtl::RtlSdrDdc warm;
    }
    ok &= requiresLock(
        "RTL DDC construction takes the single-precision lock",
        [] { return std::unique_ptr<AetherSDR::rtl::RtlSdrDdc> {}; },
        [](std::unique_ptr<AetherSDR::rtl::RtlSdrDdc>& ddc) {
            ddc = std::make_unique<AetherSDR::rtl::RtlSdrDdc>();
        },
        [] { return AetherSDR::fftwfPlannerLock(); });
    ok &= requiresLock(
        "RTL DDC destruction takes the single-precision lock",
        [] { return std::make_unique<AetherSDR::rtl::RtlSdrDdc>(); },
        [](std::unique_ptr<AetherSDR::rtl::RtlSdrDdc>& ddc) { ddc.reset(); },
        [] { return AetherSDR::fftwfPlannerLock(); });
#endif

    return ok ? 0 : 1;
}
