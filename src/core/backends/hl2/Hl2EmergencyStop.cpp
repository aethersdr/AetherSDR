#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QHostAddress>

#include <atomic>
#include <csignal>
#include <cstring>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

namespace AetherSDR::hl2 {

namespace {

// Static storage only: a handler must not touch anything whose lifetime it
// cannot reason about. A published StopTarget is IMMUTABLE: arm() fills the
// slot g_armed does not point at, then publishes it with one release store;
// fire() acquire-loads the pointer once and reads only through it. No memory
// order on a store to a single mutable payload can stop a handler whose load
// already returned it (#4581). Two slots survive one re-arm per fire, not two.
struct StopTarget {
    sockaddr_in  addr{};
    std::uint8_t packet[64]{};
    qintptr      fd{-1};   // SOCKET is UINT_PTR on Win64; narrowed only at sendto()
};

// Constant-initialised, so well-formed before main().
StopTarget g_slots[2];

// Plain, not atomic: arms are serialised (one MetisClient at a time, start()
// on its I/O thread, a backend's thread joined before the next is built). Two
// concurrent arms could fill the published slot, and an atomic index would not
// prevent it. A second radio per process needs a target per radio.
unsigned   g_nextSlot = 0;
std::atomic<StopTarget*> g_armed{nullptr};

static_assert(std::atomic<StopTarget*>::is_always_lock_free,
              "the armed pointer is loaded from a signal handler");

// Repeats of the stop datagram. UDP, 64 bytes, and the cost of losing the only
// copy is a physical power cycle — so send it more than once.
constexpr int kStopRepeats = 3;

#ifndef Q_OS_WIN
using socket_t = int;
#else
using socket_t = SOCKET;
#endif

extern "C" void terminatingSignalHandler(int sig)
{
    fireEmergencyStop();

    // Restore the default disposition and re-raise, so the process dies exactly
    // as it would have without us: same exit status, same core dump, same
    // crash reporter. Swallowing the signal here would turn a kill into a hang.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // namespace

void armEmergencyStop(qintptr fd, const QHostAddress& host, quint16 port,
                      const std::array<std::uint8_t, 64>& stopPacket) noexcept
{
    bool ipv4 = false;
    const quint32 v4 = host.toIPv4Address(&ipv4);
    if (fd < 0 || !ipv4) {
        // Metis is IPv4-only, so a non-IPv4 address means we have nothing we
        // could send to. Disarm rather than leave a stale descriptor armed.
        disarmEmergencyStop();
        return;
    }

    // Never the slot g_armed points at: g_nextSlot advances only after a publish.
    StopTarget& slot = g_slots[g_nextSlot & 1u];
    std::memset(&slot.addr, 0, sizeof(slot.addr));
    slot.addr.sin_family = AF_INET;
    slot.addr.sin_port = htons(port);
    slot.addr.sin_addr.s_addr = htonl(v4);
    std::memcpy(slot.packet, stopPacket.data(), sizeof(slot.packet));
    slot.fd = fd;

    // Publish last, as one store, so fd never becomes visible ahead of its address.
    g_armed.store(&slot, std::memory_order_release);
    ++g_nextSlot;
}

void disarmEmergencyStop() noexcept
{
    // A handler that already loaded the pointer still makes its sends, unordered
    // against the caller's close(): from another thread it can hit a closed
    // descriptor, or whatever reused the number. Disarming before the close
    // narrows that to a handler in flight; closing it would need the closer to
    // wait for in-flight handlers, which nothing here does.
    g_armed.store(nullptr, std::memory_order_release);
}

void fireEmergencyStop() noexcept
{
    // One load; everything else is read through it.
    const StopTarget* target = g_armed.load(std::memory_order_acquire);
    if (!target) {
        return;
    }

    for (int i = 0; i < kStopRepeats; ++i) {
        // sendto() is async-signal-safe (POSIX); socket_t is the platform type.
        (void)::sendto(static_cast<socket_t>(target->fd),
                       reinterpret_cast<const char*>(target->packet),
                       sizeof(target->packet), 0,
                       reinterpret_cast<const sockaddr*>(&target->addr),
                       sizeof(target->addr));
    }
}

void installEmergencyStopSignalHandlers() noexcept
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
        // Never override an inherited SIG_IGN (POSIX): nohup ignores SIGHUP,
        // and a handler here would make it fatal again.
        const auto previous = std::signal(sig, terminatingSignalHandler);
        if (previous == SIG_IGN)
            std::signal(sig, SIG_IGN);
    }
}

}  // namespace AetherSDR::hl2
