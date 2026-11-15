// A serial transport, presented to Hamlib as a socket it can connect to.
//
// `transport.hpp` explains why this works: Hamlib turns any backend
// into a network client when its pathname parses as `host:port`, so the
// whole of `native/cmake/hamlib.cmake`'s pinned radio list is reachable
// over a socket. This is the socket. It binds a listener on loopback,
// waits for exactly one connection, and pumps bytes between it and the
// transport for as long as the session lasts.
//
// **Loopback only, and that is a security property rather than a
// default.** The far end of this socket is an unauthenticated path to
// somebody's transmitter: anything that can connect can key it. So the
// bind address is `127.0.0.1` explicitly, never `INADDR_ANY`, and the
// port is ephemeral rather than well-known so it is not a thing to
// scan for. On Android the app sandbox is a second layer, but this file
// also builds and runs on the desktop, where it is the only one.
//
// **One connection, then done.** Hamlib's `network_open` connects once
// per `rig_open`, so a disconnect means the session is over -- and
// re-accepting would mean the pump-out thread having to hand its
// descriptor over mid-flight, which is a race in exchange for a case
// that does not arise. A `RigController` reconfigure builds a new
// backend and therefore a new bridge.
//
// **A thread per direction, not one thread polling both.** The
// alternative -- `poll()` the socket briefly, then read the transport
// briefly, forever -- costs tens of wakeups a second doing nothing on a
// device where a listening session is measured in hours of battery.
// Two threads that each block cost none. What it buys is paid for by
// `SerialTransport`'s threading contract, which both Android transports
// satisfy for free.

#ifndef DATA2G_RIG_BRIDGE_HPP
#define DATA2G_RIG_BRIDGE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rig/transport.hpp"

namespace data2g::rig {

// How long a blocked transport read waits before looking at the stop
// flag again.
//
// The primary way a read is woken is `SerialTransport::close()`, which
// the contract requires -- this is the backstop for a transport that
// does not honour it, so that a shutdown is bounded rather than
// indefinite. Same shape as `tx::PttWatchdog`: the scope guard is the
// mechanism, the independent timer is for when the mechanism does not
// run. Long enough that an idle link wakes twice a second and no more.
inline constexpr int BRIDGE_READ_TIMEOUT_MS = 500;

// How long to wait for Hamlib to connect after `start()`.
//
// It connects within a millisecond or two in practice: `rig_open` is
// called immediately after, on the same thread that just read
// `endpoint()`. This bound exists so that a caller who builds a bridge
// and then fails before opening the rig does not leave a thread parked
// forever.
inline constexpr double BRIDGE_ACCEPT_TIMEOUT_S = 30.0;

class LoopbackBridge {
public:
    explicit LoopbackBridge(std::shared_ptr<SerialTransport> transport);

    // Stops and joins. Safe with the pump running.
    ~LoopbackBridge();

    LoopbackBridge(const LoopbackBridge&) = delete;
    LoopbackBridge& operator=(const LoopbackBridge&) = delete;

    // Bind, listen, and start pumping. The transport is opened here,
    // on the calling thread, so a device that cannot be acquired fails
    // before anything else is built -- and it fails on the rig worker,
    // which is the only thread allowed to block on hardware.
    //
    // Throws RigError. Binding and listening happen *before* this
    // returns, so `endpoint()` is valid the moment it does.
    void start();

    // "127.0.0.1:54321" -- what to hand Hamlib as the rig pathname.
    // Empty before `start()`.
    std::string endpoint() const;
    int port() const;

    // Idempotent, and safe to call from a thread other than the one
    // that started it.
    void stop() noexcept;

    // Whether the pump is still carrying traffic. False before a client
    // connects and after either side goes away -- so a caller polling
    // this can tell a rig that hung up from one that is merely quiet.
    bool connected() const;

    // Why the pump stopped, empty if it has not. Reported rather than
    // thrown: it happens on a pump thread, where there is no call to
    // fail.
    std::string last_error() const;

private:
    void pump_in();   // socket -> transport
    void pump_out();  // transport -> socket
    void set_error(const std::string& what);

    std::shared_ptr<SerialTransport> transport_;

    // `intptr_t` rather than a platform socket type so this header
    // stays free of <winsock2.h>, which drags in <windows.h> and its
    // `min` and `max` macros -- CLAUDE.md's rule about never letting
    // that into a widely-included header. Windows' `SOCKET` is a
    // `UINT_PTR`, so this is the one integer type that round-trips it
    // on both platforms; -1 is the sentinel, which is also what
    // `INVALID_SOCKET` casts to. The .cpp does the conversion in one
    // place.
    std::atomic<std::intptr_t> listen_fd_{-1};
    std::atomic<std::intptr_t> client_fd_{-1};
    std::atomic<int> port_{0};

    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};

    mutable std::mutex mu_;
    // Serialises stop() against itself. Only that: joining one thread
    // from two callers at once is undefined, and the destructor and a
    // caller's explicit stop() are two callers.
    mutable std::mutex stop_mu_;
    std::condition_variable cv_;
    std::string error_;
    std::string endpoint_;

    std::thread in_;
    std::thread out_;
};

// Whether Hamlib will read `device` as a network address rather than a
// serial port -- **the same question `parse_hoststr()` in Hamlib's
// `src/misc.c` answers**, and deliberately the same answer.
//
// It matters because the two decisions have to agree. If the app
// concludes "this is a device, open a transport and bridge it" while
// Hamlib concludes "this is a hostname", the operator gets a connection
// attempt to a machine that does not exist while a perfectly good radio
// sits idle -- and the reverse leaves a bridge nobody connects to. The
// rules are Hamlib's, not ours: anything containing `/` is a path,
// anything starting `com` is a Windows port, an escaped `\\.\COM3` is
// one too, and what is left needs a colon with a number after it.
bool is_network_device(const std::string& device);

}  // namespace data2g::rig

#endif
