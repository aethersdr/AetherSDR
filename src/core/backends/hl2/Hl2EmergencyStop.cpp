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

// Plain globals, not a class: a signal handler must not touch anything whose
// lifetime it cannot reason about, and these have static storage duration for
// the whole process.
//
// THE PAYLOAD IS IMMUTABLE ONCE PUBLISHED, and that is the synchronisation.
// arm() fills a slot NO HANDLER CAN BE LOOKING AT, then publishes it with a
// single release store; fire() acquire-loads the pointer once and reads only
// through it. That is what makes a handler firing halfway through arm() see
// either the complete previous target or the complete new one — never a
// descriptor paired with someone else's address.
//
// THAT GUARANTEE USED TO BE CLAIMED HERE AND NOT DELIVERED, which is the defect
// aethersdr/AetherSDR#4581 is about. The previous shape kept ONE mutable
// payload: arm() stored -1 into the descriptor "to stop any concurrent fire()",
// then memset the address in place, then republished. It does not stop a
// handler whose acquire-load had ALREADY returned the previous descriptor — an
// atomic store cannot reach back and change a load that has happened — and that
// handler went on to read the address while the memset was running. Signals are
// delivered to an arbitrary thread, so the handler need not be the arming one.
//
// No memory order on the disarming store closes that. An ordering constrains
// which other writes a load is guaranteed to see ALONGSIDE a value; it does not
// constrain WHICH VALUE a racing load may return. Not mutating what a handler
// may still be reading is what closes it.
//
// TWO SLOTS ARE ENOUGH. Reaching the slot a handler is reading would take two
// further arms — two reconnects — inside the few microseconds of three sendto()
// calls, in a process that is already on its way out because a terminating
// signal has been delivered.
struct StopTarget {
    sockaddr_in  addr{};
    std::uint8_t packet[64]{};
    // qintptr, not int. The value arrives from QUdpSocket::socketDescriptor() as
    // qintptr and is used as SOCKET (UINT_PTR) on Windows, so an int here
    // narrowed a 64-bit handle on Win64. Windows hands out small socket handles
    // in practice, so this was never observed to bite; it is fixed because it
    // costs nothing. The cast to the platform's own type happens at the sendto()
    // call and nowhere else. (#4581)
    qintptr      fd{-1};
};

// Every member is constant-initialised, so these are alive before main() and a
// handler installed at any point has something well-formed to read.
StopTarget g_slots[2];
unsigned   g_nextSlot = 0;                    // arm() only; never read by a handler
std::atomic<StopTarget*> g_armed{nullptr};

// A handler may only touch lock-free atomics, and this pointer load is the
// entire synchronisation budget fire() has.
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

    // Fill the slot that is NOT the published one. g_nextSlot advances only
    // here, and only after a publish, so the slot written below is never the
    // slot g_armed points at — nothing a handler can be reading is touched.
    StopTarget& slot = g_slots[g_nextSlot & 1u];
    std::memset(&slot.addr, 0, sizeof(slot.addr));
    slot.addr.sin_family = AF_INET;
    slot.addr.sin_port = htons(port);
    slot.addr.sin_addr.s_addr = htonl(v4);
    std::memcpy(slot.packet, stopPacket.data(), sizeof(slot.packet));
    slot.fd = fd;

    // Publish LAST, and as a SINGLE store. Everything above must be visible to a
    // handler that sees this pointer, and the descriptor must not become visible
    // ahead of the address it belongs to.
    g_armed.store(&slot, std::memory_order_release);
    ++g_nextSlot;
}

void disarmEmergencyStop() noexcept
{
    // A handler that has already loaded the pointer still completes its sends;
    // that is harmless, because the target it holds is complete and the socket
    // is still open until MetisClient::stop() closes it — which is why stop()
    // disarms before the close and not after.
    g_armed.store(nullptr, std::memory_order_release);
}

void fireEmergencyStop() noexcept
{
    // ONE load, and everything else read through the pointer it returned. Loading
    // the target twice would reintroduce exactly what #4581 was about.
    const StopTarget* target = g_armed.load(std::memory_order_acquire);
    if (!target) {
        return;
    }

    for (int i = 0; i < kStopRepeats; ++i) {
        // sendto() is on POSIX's async-signal-safe list. Nothing else in this
        // function allocates, locks, or calls into Qt — that is the whole
        // reason the payload was built in advance.
        //
        // The descriptor is narrowed HERE and nowhere else: socket_t is int on
        // POSIX and SOCKET (UINT_PTR) on Windows, and this call is the one place
        // the platform's own type is the correct one.
        (void)::sendto(static_cast<socket_t>(target->fd),
                       reinterpret_cast<const char*>(target->packet),
                       sizeof(target->packet), 0,
                       reinterpret_cast<const sockaddr*>(&target->addr),
                       sizeof(target->addr));
    }
}

void installEmergencyStopSignalHandlers() noexcept
{
    // SIGTERM  — plain `kill`, and what a service manager or the OS sends.
    // SIGINT   — Ctrl-C from a terminal-launched run.
    // SIGHUP   — the controlling terminal went away.
    // SIGQUIT  — Ctrl-backslash.
    //
    // NOT SIGKILL: it cannot be caught, so `kill -9` still wedges the radio.
    //
    // TERMINATION SIGNALS ONLY — deliberately not SIGSEGV/SIGABRT/SIGBUS. A
    // crash leaves the radio in the same state and it is tempting to cover it
    // here, but those signals belong to whatever crash reporting the platform
    // and the app already have (on macOS the reporter uses Mach exception
    // ports, and MacStartupAbortGuard owns SIGABRT during startup). Quietly
    // taking them over to save a power cycle is not a trade worth making.
    static const int kSignals[] = {
        SIGTERM, SIGINT,
#ifndef Q_OS_WIN
        SIGHUP, SIGQUIT,
#endif
    };
    for (const int sig : kSignals) {
        // NEVER override an inherited SIG_IGN.
        //
        // POSIX is explicit that a process which inherits a signal as ignored
        // should leave it that way, and this is not a theoretical rule: nohup
        // works by ignoring SIGHUP, so installing a handler over it converts a
        // signal the parent deliberately neutralised back into a fatal one.
        // Caught in testing — a nohup'd run died the moment its launching shell
        // exited, which is the exact opposite of what nohup is for.
        const auto previous = std::signal(sig, terminatingSignalHandler);
        if (previous == SIG_IGN)
            std::signal(sig, SIG_IGN);
    }
}

}  // namespace AetherSDR::hl2
