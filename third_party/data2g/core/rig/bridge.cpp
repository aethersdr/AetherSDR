#include "rig/bridge.hpp"

#include "rig/trace.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>

// **POSIX sockets, and this file is not built on Windows.** See
// `native/CMakeLists.txt` for the decision; the short version is that
// the bridge exists because Android hands an app no serial device, and
// Windows has COM ports, so nothing there ever constructs one.
//
// A Winsock port is not a matter of swapping the headers, which is why
// the shim that used to be here was removed rather than left as a
// starting point. `stop()` relies on `shutdown()` waking a thread
// blocked in `recv()` -- guaranteed on POSIX, and **not** how Winsock
// behaves: a blocked `recv` there does not return, and Microsoft warns
// against `closesocket` concurrently with a blocking call. So a Windows
// implementation has to make the data path non-blocking and poll it,
// the way `pump_in`'s accept loop already does, rather than translating
// call for call. It hung for two minutes in CI before that was
// understood.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace data2g::rig {
namespace {

using socket_t = int;
using poll_fd = struct pollfd;

int poll_sockets(poll_fd* fds, unsigned n, int timeout_ms) {
    return ::poll(fds, n, timeout_ms);
}
void close_socket(socket_t s) { ::close(s); }
int socket_errno() { return errno; }
constexpr int kShutBoth = SHUT_RDWR;

constexpr std::intptr_t kNoFd = -1;

socket_t as_socket(std::intptr_t v) { return static_cast<socket_t>(v); }
std::intptr_t as_handle(socket_t s) { return static_cast<std::intptr_t>(s); }

std::string socket_error_text(const char* what) {
    return std::string(what) + " failed (errno " + std::to_string(socket_errno()) + ")";
}

}  // namespace

// ---------------------------------------------------------------------------

bool is_network_device(const std::string& device) {
    // Hamlib's `parse_hoststr` (src/misc.c), rule for rule. The
    // exclusions are the whole of it: a path, a Windows COM port, or an
    // escaped UNC-style COM port is a serial device, and **everything
    // else is a network address** -- including a bare hostname with no
    // colon and no port, which is the part that surprises people. That
    // is genuinely how Hamlib reads it (the final branch accepts a
    // one-conversion `%255[^:]` match), and matching it is the point:
    // disagreeing here means the app opens a bridge Hamlib will not
    // connect to, or dials a hostname while a radio sits idle.
    if (device.empty()) return false;
    if (device.find('/') != std::string::npos) return false;
    if (device.find("\\.\\") != std::string::npos) return false;
    if (device.size() >= 3) {
        std::string head = device.substr(0, 3);
        std::transform(head.begin(), head.end(), head.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (head == "com") return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

LoopbackBridge::LoopbackBridge(std::shared_ptr<SerialTransport> transport)
    : transport_(std::move(transport)) {
    if (!transport_) throw RigError("bridge: no transport");
}

LoopbackBridge::~LoopbackBridge() { stop(); }

void LoopbackBridge::start() {
    // The transport first: if the cable is gone there is no point
    // holding a port open, and this is the failure an operator most
    // needs named.
    transport_->open();

    socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == static_cast<socket_t>(kNoFd)) {
        transport_->close();
        throw RigError(socket_error_text("bridge: socket"));
    }

    // **127.0.0.1, never INADDR_ANY.** The far end of this is an
    // unauthenticated path to a transmitter; anything that can reach
    // the port can key the radio. Port 0 asks the kernel for an
    // ephemeral one, so there is nothing fixed to scan for either.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    auto give_up = [&](const char* what) {
        close_socket(listener);
        transport_->close();
        throw RigError(socket_error_text(what));
    };

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        give_up("bridge: bind");
    }
    if (::listen(listener, 1) != 0) give_up("bridge: listen");

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        give_up("bridge: getsockname");
    }

    const int port = ntohs(bound.sin_port);
    listen_fd_.store(as_handle(listener));
    port_.store(port);
    {
        std::lock_guard<std::mutex> lock(mu_);
        endpoint_ = "127.0.0.1:" + std::to_string(port);
    }

    trace("bridge: listening on 127.0.0.1:" + std::to_string(port) +
          ", transport open");

    stopping_.store(false);
    in_ = std::thread([this] { pump_in(); });
    out_ = std::thread([this] { pump_out(); });
}

std::string LoopbackBridge::endpoint() const {
    std::lock_guard<std::mutex> lock(mu_);
    return endpoint_;
}

int LoopbackBridge::port() const { return port_.load(); }

bool LoopbackBridge::connected() const { return connected_.load(); }

std::string LoopbackBridge::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void LoopbackBridge::set_error(const std::string& what) {
    std::lock_guard<std::mutex> lock(mu_);
    // First one wins: the first failure is the cause and everything
    // after it is the consequence, which is the wrong thing to show an
    // operator.
    if (error_.empty()) error_ = what;
}

void LoopbackBridge::stop() noexcept {
    // **No early return on "already stopping".** A pump thread sets
    // `stopping_` itself when the far end hangs up, so by the time the
    // owner calls stop() the flag is routinely already true -- and an
    // early return there would skip closing the listener and the
    // transport, leaking a descriptor and a USB claim for every session
    // that ended by itself rather than by request. Everything below is
    // written to be idempotent instead; `stop_mu_` is only here because
    // joining one thread from two callers at once is undefined.
    std::lock_guard<std::mutex> once(stop_mu_);
    {
        // Under `mu_`, not merely atomic: `pump_out` waits on `cv_` with
        // this predicate, so setting it outside the lock loses the
        // wakeup when the store lands between its test and its wait.
        std::lock_guard<std::mutex> lock(mu_);
        stopping_.store(true);
    }
    cv_.notify_all();

    // Wake pump_in out of recv/accept. `shutdown` rather than a bare
    // close, because closing a descriptor another thread is blocked on
    // is not portable: the fd number can be reused before the blocked
    // call notices, and then the wakeup lands on a stranger.
    const std::intptr_t client = client_fd_.load();
    if (client != kNoFd) ::shutdown(as_socket(client), kShutBoth);

    // Wake pump_out out of a blocked read. This is the contract; the
    // read timeout is only the backstop for a transport that ignores it.
    if (transport_) transport_->close();

    if (in_.joinable()) in_.join();
    if (out_.joinable()) out_.join();

    // Only once nobody can be inside them.
    const std::intptr_t listener = listen_fd_.exchange(kNoFd);
    if (listener != kNoFd) close_socket(as_socket(listener));
    const std::intptr_t c = client_fd_.exchange(kNoFd);
    if (c != kNoFd) close_socket(as_socket(c));
    connected_.store(false);
}

void LoopbackBridge::pump_in() {
    const socket_t listener = as_socket(listen_fd_.load());

    // Accept with a deadline rather than a blocking accept, so that a
    // caller who builds a bridge and then fails before opening the rig
    // does not leave this thread parked for the life of the process.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(BRIDGE_ACCEPT_TIMEOUT_S);
    socket_t client = static_cast<socket_t>(kNoFd);
    while (!stopping_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            set_error("bridge: nothing connected");
            break;
        }
        poll_fd pfd{};
        pfd.fd = listener;
        pfd.events = POLLIN;
        const int rc = poll_sockets(&pfd, 1, 100);
        if (rc < 0) {
            if (socket_errno() == EINTR) continue;
            set_error(socket_error_text("bridge: poll"));
            break;
        }
        if (rc == 0) continue;
        client = ::accept(listener, nullptr, nullptr);
        if (client == static_cast<socket_t>(kNoFd)) {
            if (stopping_.load()) break;
            set_error(socket_error_text("bridge: accept"));
            break;
        }
        break;
    }

    if (client == static_cast<socket_t>(kNoFd)) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_.store(true);
        }
        cv_.notify_all();
        return;
    }

    // **Nagle off.** CAT is a stream of very short writes that each
    // expect a reply, which is precisely the traffic Nagle delays: it
    // would hold a 4-byte command back waiting for more to send, and
    // the symptom is a radio that reads as slow or intermittent rather
    // than as a socket option.
    int one = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    client_fd_.store(as_handle(client));
    {
        std::lock_guard<std::mutex> lock(mu_);
        connected_.store(true);
    }
    cv_.notify_all();

    std::array<std::uint8_t, 512> buf{};
    while (!stopping_.load()) {
        const auto n = ::recv(client, reinterpret_cast<char*>(buf.data()),
                              static_cast<int>(buf.size()), 0);
        if (n == 0) break;  // Hamlib closed the rig; the session is over.
        if (n < 0) {
            if (socket_errno() == EINTR) continue;
            if (!stopping_.load()) set_error(socket_error_text("bridge: recv"));
            break;
        }
        try {
            transport_->write(buf.data(), static_cast<std::size_t>(n));
            // **After the write, not before.** The whole point of this
            // line is that it is only reached when the transport
            // accepted the bytes, which is the fact Hamlib's own trace
            // cannot report: it sees a successful socket write and has
            // no idea whether anything left the USB port. Logged on the
            // way in, it would say "delivered" for a write that threw --
            // the false reassurance this was added to remove.
            if (tracing()) {
                trace("bridge: -> rig " + std::to_string(n) + ": " +
                      hex_bytes(buf.data(), static_cast<std::size_t>(n)));
            }
        } catch (const std::exception& e) {
            set_error(std::string("bridge: write to rig: ") + e.what());
            break;
        }
    }

    // One direction ending ends the session: a half-open bridge would
    // let a poll succeed against a link that can no longer answer.
    {
        std::lock_guard<std::mutex> lock(mu_);
        connected_.store(false);
        stopping_.store(true);
    }
    cv_.notify_all();
}

void LoopbackBridge::pump_out() {
    {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return connected_.load() || stopping_.load(); });
    }

    std::array<std::uint8_t, 512> buf{};
    while (!stopping_.load()) {
        std::size_t n = 0;
        try {
            n = transport_->read(buf.data(), buf.size(), BRIDGE_READ_TIMEOUT_MS);
        } catch (const std::exception& e) {
            if (!stopping_.load()) set_error(std::string("bridge: read from rig: ") + e.what());
            break;
        }
        if (n == 0) continue;  // An idle radio, not a failure.
        if (tracing()) {
            trace("bridge: <- rig " + std::to_string(n) + ": " +
                  hex_bytes(buf.data(), n));
        }

        const std::intptr_t handle = client_fd_.load();
        if (handle == kNoFd) break;
        const socket_t client = as_socket(handle);

        std::size_t sent = 0;
        while (sent < n) {
            const auto wrote = ::send(client,
                                      reinterpret_cast<const char*>(buf.data() + sent),
                                      static_cast<int>(n - sent), 0);
            if (wrote <= 0) {
                if (wrote < 0 && socket_errno() == EINTR) continue;
                if (!stopping_.load()) set_error(socket_error_text("bridge: send"));
                sent = n;  // give up on this block
                stopping_.store(true);
                break;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    if (tracing()) {
        const std::string why = last_error();
        trace("bridge: stopped reading from the rig" +
              (why.empty() ? std::string(" (session ended)") : ": " + why));
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_.store(true);
    }
    cv_.notify_all();
}

}  // namespace data2g::rig
