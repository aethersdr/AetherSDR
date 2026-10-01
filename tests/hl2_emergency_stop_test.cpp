// The arm / disarm / fire path of the signal-handler emergency stop.
// (aethersdr/AetherSDR#4581)
//
// TWO PARTS, and they prove different things.
//
// PART 1 is a guard on the data structure. A target published by
// armEmergencyStop() is the one fireEmergencyStop() sends to, a RE-ARM moves
// the descriptor, the address and the packet together, and a disarm — explicit,
// or by arming with an fd or an address that cannot be used — silences it.
// Nothing in it is concurrent, so it passes on the tree before #4581's fix as
// well. Its mutation is an off-by-one in the published slot index.
//
// PART 2 is the falsifier for #4581 itself. One thread re-arms while another
// fires, which is what a terminating signal delivered to a second thread during
// a reconnect does. Exactly ONE re-arm overlaps each fire, and the two targets
// are told apart by the datagram alone: every byte of a target's packet is the
// same value, and its low bit names the socket it was armed with. A datagram on
// the wrong socket, or one with mixed bytes, is a packet paired with an address
// it was never armed with. Before the fix arm() rewrote the one payload in
// place and this section counts such datagrams; after it, a handler reads a
// slot arm() does not write, and the count is zero.
//
// WHAT PART 2 IS NOT.
//
//   - It is not deterministic on the unfixed tree. The mismatch needs the
//     re-arm to land between sendto()'s copy of the address and its copy of
//     the payload. On a host with one core the threads never overlap and it
//     passes vacuously. It cannot go red on a correct tree for that reason:
//     the assertion is "no mismatched datagram", not "the overlap happened".
//   - It holds the fix to its stated bound and no further: one re-arm per
//     fire. Two slots do not survive two re-arms inside one fire, and the
//     source says so.
//   - No signal is raised. fireEmergencyStop() is called from a plain thread.
//     The fixture that killed a real process (hl2_signal_stop_test) is retired;
//     see tests.cmake.
//   - It does not exercise the Win64 SOCKET narrowing. That needs a descriptor
//     above INT_MAX, which this platform does not hand out.
//
// Under ThreadSanitizer part 2 is clean on the fixed tree (macOS, clang). On
// the unfixed tree the sanitizer itself stayed silent there, because the racing
// reads happen inside sendto(); the datagram check is what went red.
//
// SOCKETS: three UDP sockets on 127.0.0.1, ports chosen by the kernel. Two are
// sinks for our own sendto() and one is the descriptor that gets armed. None
// stands in for a radio. A failed bind exits 77 (skipped).

#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QUdpSocket>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::fprintf(stderr, "[ OK ] %s\n", what);
    }
}

static std::array<std::uint8_t, 64> markedPacket(std::uint8_t marker)
{
    std::array<std::uint8_t, 64> p{};
    p.fill(marker);
    return p;
}

// Collect whatever arrived, waiting only as long as it takes. Loopback UDP is
// not guaranteed, so the count is asserted as "at least one of the three
// repeats", never as exactly three — the repeats exist because the datagram may
// be lost, and a test that demanded all three would be asserting the opposite.
static std::vector<QByteArray> drain(QUdpSocket& sock, int budgetMs)
{
    std::vector<QByteArray> out;
    while (sock.waitForReadyRead(budgetMs)) {
        while (sock.hasPendingDatagrams()) {
            out.push_back(sock.receiveDatagram().data());
        }
        budgetMs = 20;   // the first wait pays the latency; the rest are drains
    }
    return out;
}

static bool allAre(const std::vector<QByteArray>& got, std::uint8_t marker)
{
    if (got.empty()) {
        return false;
    }
    for (const QByteArray& d : got) {
        if (d.size() != 64) {
            return false;
        }
        for (char c : d) {
            if (static_cast<std::uint8_t>(c) != marker) {
                return false;
            }
        }
    }
    return true;
}

// Everything already queued on the socket, without waiting.
static std::vector<QByteArray> pending(QUdpSocket& sock)
{
    std::vector<QByteArray> out;
    while (sock.hasPendingDatagrams()) {
        out.push_back(sock.receiveDatagram().data());
    }
    return out;
}

// A datagram is a complete target when all 64 bytes are one value and that
// value's low bit names the socket it arrived on.
static int mismatched(const std::vector<QByteArray>& got, int socketIndex)
{
    int bad = 0;
    for (const QByteArray& d : got) {
        bool whole = d.size() == 64;
        for (int i = 1; whole && i < d.size(); ++i) {
            whole = d[i] == d[0];
        }
        if (!whole || (static_cast<std::uint8_t>(d[0]) & 1u) != static_cast<unsigned>(socketIndex)) {
            ++bad;
        }
    }
    return bad;
}

// A short busy delay, so the re-arm lands at a different point of the fire in
// each round. Deliberately not a sleep: the window is well under a microsecond.
static void spin(std::uint32_t& state, std::uint32_t mask)
{
    state = state * 1664525u + 1013904223u;
    volatile std::uint32_t sink = 0;
    for (std::uint32_t i = (state >> 16) & mask; i > 0; --i) {
        sink = sink + i;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QUdpSocket first;
    QUdpSocket second;
    if (!first.bind(QHostAddress::LocalHost, 0) || !second.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind a UDP socket on 127.0.0.1\n");
        return 77;
    }
    const quint16 firstPort = first.localPort();
    const quint16 secondPort = second.localPort();

    QUdpSocket sender;
    if (!sender.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind a UDP socket on 127.0.0.1\n");
        return 77;
    }
    const qintptr fd = sender.socketDescriptor();
    check(fd >= 0, "sending socket has a descriptor to arm with");

    // ---- armed target is the one that receives ----
    armEmergencyStop(fd, QHostAddress::LocalHost, firstPort, markedPacket(0xA1));
    fireEmergencyStop();
    check(allAre(drain(first, 500), 0xA1), "the armed target receives the stop datagram");
    check(drain(second, 20).empty(), "nothing reaches an address that was never armed");

    // ---- a re-arm moves the WHOLE target, not part of it ----
    //
    // Different port AND different payload, so a slot published while another
    // slot was filled shows up as either the old address or the old bytes.
    armEmergencyStop(fd, QHostAddress::LocalHost, secondPort, markedPacket(0xB2));
    fireEmergencyStop();
    check(allAre(drain(second, 500), 0xB2), "a re-arm moves the address and the packet together");
    check(drain(first, 20).empty(), "the previous target stops receiving");

    // ---- a third arm reuses the first slot ----
    armEmergencyStop(fd, QHostAddress::LocalHost, firstPort, markedPacket(0xC3));
    fireEmergencyStop();
    check(allAre(drain(first, 500), 0xC3), "the slots alternate without carrying stale bytes");
    check(drain(second, 20).empty(), "the second target stops receiving");

    // ---- disarm silences it ----
    disarmEmergencyStop();
    fireEmergencyStop();
    check(drain(first, 100).empty() && drain(second, 20).empty(),
          "a disarmed stop sends nothing");

    // ---- arming with an unusable fd disarms rather than leaving the old one ----
    armEmergencyStop(fd, QHostAddress::LocalHost, firstPort, markedPacket(0xD4));
    armEmergencyStop(-1, QHostAddress::LocalHost, firstPort, markedPacket(0xD4));
    fireEmergencyStop();
    check(drain(first, 100).empty(), "an invalid descriptor disarms instead of leaving the last target");

    // ---- and so does a non-IPv4 destination: Metis is IPv4-only ----
    armEmergencyStop(fd, QHostAddress::LocalHost, firstPort, markedPacket(0xE5));
    armEmergencyStop(fd, QHostAddress(QStringLiteral("::1")), firstPort, markedPacket(0xE5));
    fireEmergencyStop();
    check(drain(first, 100).empty(), "a non-IPv4 destination disarms");

    disarmEmergencyStop();

    // ---- PART 2: one re-arm overlapping one fire (#4581) ----
    //
    // Round r arms target (r & 1): that socket's port, and a packet whose every
    // byte is (2r | (r & 1)). The worker does the arming and this thread does
    // the firing; the round counter and the acknowledgement are the only
    // coordination, so each fire overlaps at most the one re-arm of its round.
    {
        constexpr int kRounds = 20000;
        const quint16 ports[2] = {firstPort, secondPort};
        QUdpSocket* sinks[2] = {&first, &second};

        armEmergencyStop(fd, QHostAddress::LocalHost, ports[0], markedPacket(0x00));
        (void)pending(first);
        (void)pending(second);

        std::atomic<int> go{0};
        std::atomic<int> armed{0};
        std::thread rearm([&]() {
            std::uint32_t rng = 0x9E3779B9u;
            for (int r = 1; r <= kRounds; ++r) {
                while (go.load(std::memory_order_acquire) != r) {
                    std::this_thread::yield();
                }
                spin(rng, 0x3FFu);
                const int k = r & 1;
                armEmergencyStop(fd, QHostAddress::LocalHost, ports[k],
                                 markedPacket(static_cast<std::uint8_t>((r << 1) | k)));
                armed.store(r, std::memory_order_release);
            }
        });

        int bad = 0;
        int seen = 0;
        std::uint32_t rng = 0x7F4A7C15u;
        for (int r = 1; r <= kRounds; ++r) {
            go.store(r, std::memory_order_release);
            spin(rng, 0x3FFu);
            fireEmergencyStop();
            while (armed.load(std::memory_order_acquire) != r) {
                std::this_thread::yield();
            }
            for (int k = 0; k < 2; ++k) {
                const std::vector<QByteArray> got = pending(*sinks[k]);
                seen += static_cast<int>(got.size());
                bad += mismatched(got, k);
            }
        }
        rearm.join();
        disarmEmergencyStop();

        // Loopback delivery is not promised to be synchronous everywhere, so
        // collect what is still in flight. The judgement needs no round number.
        for (int k = 0; k < 2; ++k) {
            const std::vector<QByteArray> got = drain(*sinks[k], 50);
            seen += static_cast<int>(got.size());
            bad += mismatched(got, k);
        }

        std::fprintf(stderr, "concurrent re-arm: %d rounds, %d datagrams, %d mismatched\n",
                     kRounds, seen, bad);
        check(seen > 0, "the firing thread's datagrams arrive");
        check(bad == 0, "a fire overlapping a re-arm never sends a packet to an address it was not armed with");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "hl2_emergency_stop_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
