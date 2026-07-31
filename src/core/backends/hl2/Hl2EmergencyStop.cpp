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
// The armed flag is the ONLY synchronisation. arm() fills the payload first and
// publishes the descriptor last with a release store; fire() acquires the
// descriptor before reading anything else. That ordering is what makes a
// handler firing halfway through arm() see either the complete previous state
// or nothing at all — never a descriptor paired with someone else's address.
std::atomic<int>  g_fd{-1};
sockaddr_in       g_addr{};
std::uint8_t      g_packet[64]{};

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

    g_fd.store(-1, std::memory_order_relaxed);   // stop any concurrent fire()

    std::memset(&g_addr, 0, sizeof(g_addr));
    g_addr.sin_family = AF_INET;
    g_addr.sin_port = htons(port);
    g_addr.sin_addr.s_addr = htonl(v4);
    std::memcpy(g_packet, stopPacket.data(), sizeof(g_packet));

    // Publish LAST. Everything above must be visible to a handler that sees
    // this store.
    g_fd.store(static_cast<int>(fd), std::memory_order_release);
}

void disarmEmergencyStop() noexcept
{
    g_fd.store(-1, std::memory_order_release);
}

void fireEmergencyStop() noexcept
{
    const int fd = g_fd.load(std::memory_order_acquire);
    if (fd < 0)
        return;

    for (int i = 0; i < kStopRepeats; ++i) {
        // sendto() is on POSIX's async-signal-safe list. Nothing else in this
        // function allocates, locks, or calls into Qt — that is the whole
        // reason the payload was built in advance.
        (void)::sendto(static_cast<socket_t>(fd),
                       reinterpret_cast<const char*>(g_packet), sizeof(g_packet),
                       0, reinterpret_cast<const sockaddr*>(&g_addr),
                       sizeof(g_addr));
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
