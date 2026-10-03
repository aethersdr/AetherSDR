// The arm / disarm / fire path of the signal-handler emergency stop (#4581).
// PART 1: a published target is the one fired at; a re-arm moves descriptor,
// address and packet together; disarm (explicit, bad fd, non-IPv4) silences it.
// PART 2: one thread re-arms while another fires, one re-arm per fire (the
// two-slot bound); every packet byte names the socket it was armed for, so a
// torn or misaddressed datagram is counted. Timing-dependent: it can only pass
// vacuously, never fail, on a correct tree. No signal is raised here (that is
// the opt-in hl2_signal_stop_test) and the Win64 handle width is not exercised.
// SOCKETS: three UDP sockets on 127.0.0.1, kernel-chosen ports: two sinks and
// the armed sender. No fake radio. A failed bind or no descriptor exits 77.

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
    // Not a check(): with no descriptor every arm below takes the disarm path
    // and every later check fails for a reason none of them names.
    const qintptr fd = sender.socketDescriptor();
    if (fd < 0) {
        std::fprintf(stderr, "SKIP: bound UDP socket exposes no descriptor to arm with\n");
        return 77;
    }

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
    // Both drains run before either is judged: && would skip the second when
    // the first found something, and the verdict would cover one socket.
    const bool firstQuiet = drain(first, 100).empty();
    const bool secondQuiet = drain(second, 20).empty();
    check(firstQuiet && secondQuiet, "a disarmed stop sends nothing");

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

    // ---- PART 2: one re-arm overlapping one fire ----
    //
    // Round r arms target (r & 1): that socket's port, and a packet whose every
    // byte is (2r | (r & 1)). The go/armed handshake keeps each fire overlapping
    // at most the one re-arm of its round.
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
