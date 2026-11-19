#include "core/FatalSignalRelease.h"

#include <QtGlobal>

#include <atomic>
#include <csignal>

#ifndef Q_OS_WIN
#  include <signal.h>
#endif

namespace AetherSDR {

namespace {

// Static, lock-free slots: the handler reads them, so nothing here may lock or
// allocate. One slot per family that can hold a radio, with room to spare.
constexpr int kMaxReleases = 8;
std::atomic<FatalSignalReleaseFn> g_releases[kMaxReleases];

static_assert(std::atomic<FatalSignalReleaseFn>::is_always_lock_free,
              "the release table is read from a signal handler");

extern "C" void terminatingSignalHandler(int sig)
{
    fireFatalSignalReleaseAndReraise(sig);
}

}  // namespace

bool registerFatalSignalRelease(FatalSignalReleaseFn fn) noexcept
{
    if (!fn) {
        return false;
    }
    for (auto& slot : g_releases) {
        FatalSignalReleaseFn expected = nullptr;
        if (slot.compare_exchange_strong(expected, fn, std::memory_order_acq_rel)
            || expected == fn) {
            return true;
        }
    }
    return false;
}

bool isFatalSignalReleaseRegistered(FatalSignalReleaseFn fn) noexcept
{
    if (!fn) {
        return false;
    }
    for (const auto& slot : g_releases) {
        if (slot.load(std::memory_order_acquire) == fn) {
            return true;
        }
    }
    return false;
}

void fireFatalSignalRelease() noexcept
{
    for (const auto& slot : g_releases) {
        if (const FatalSignalReleaseFn fn = slot.load(std::memory_order_acquire)) {
            fn();
        }
    }
}

void fireFatalSignalReleaseAndReraise(int sig) noexcept
{
    fireFatalSignalRelease();

    // Restore the default disposition and re-raise, so the process dies exactly
    // as it would have without us: same exit status, same core dump, same
    // crash reporter. Swallowing the signal here would turn a kill into a hang.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

bool installSignalHandlerUnlessIgnored(int sig, void (*handler)(int)) noexcept
{
#ifdef Q_OS_WIN
    const auto previous = std::signal(sig, handler);
    if (previous == SIG_IGN) {
        std::signal(sig, SIG_IGN);
        return false;
    }
    return true;
#else
    // Read before writing, so an ignored signal never has a handler at all.
    struct sigaction current {};
    sigaction(sig, nullptr, &current);
    if (current.sa_handler == SIG_IGN) {
        return false;
    }
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(sig, &action, nullptr);
    return true;
#endif
}

void installFatalSignalRelease() noexcept
{
    // Termination signals only. SIGSEGV/SIGABRT/SIGBUS belong to the platform
    // crash reporting (Mach exception ports on macOS; MacStartupAbortGuard owns
    // SIGABRT during startup).
    static const int kSignals[] = {
        SIGTERM, SIGINT,
#ifndef Q_OS_WIN
        SIGHUP, SIGQUIT,
#endif
    };
    for (const int sig : kSignals) {
        installSignalHandlerUnlessIgnored(sig, terminatingSignalHandler);
    }
}

}  // namespace AetherSDR
