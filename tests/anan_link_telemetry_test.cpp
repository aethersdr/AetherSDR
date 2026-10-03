// aetherd ANAN P2 -- transport counters and the LinkStats snapshot.
//
// The status bar's "Network:" field was blank on an ANAN because the backend
// published no LinkStats at all. The counters behind it live on P2Client, are
// written only from its own I/O thread, and reach the backend as a periodic
// snapshot rather than a cross-thread read. What this pins:
//
//   * EVERY datagram counts, including the ones every parser rejects. The
//     readout describes the TRANSPORT, so a session whose IQ has stalled while
//     Status packets still arrive must not read as a dead link -- and counting
//     after the shape checks would make exactly those datagrams invisible.
//   * Sent bytes are counted on every send path, including the speaker stream,
//     which does not go through sendTo().
//   * start() resets the session, so a reconnect does not inherit totals.
//
// Loopback send only (127.0.0.1), no radio and no listener: the assertions are
// about the client's own counters, not about what arrives.

#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/P2Protocol.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace AetherSDR::anan {
// P2Client declares this a friend precisely so the counters can be pinned
// without widening its public surface (same pattern as the sibling ANAN tests).
struct P2ClientTestAccess {
    static void feedDatagram(P2Client& c, std::span<const std::uint8_t> bytes,
                             quint16 port)
    {
        c.handleDatagram(bytes, port);
    }
    static void publish(P2Client& c) { c.publishLinkCounters(); }
    static quint64 rxBytes(const P2Client& c) { return c.m_rxBytes; }
    static quint64 rxPackets(const P2Client& c) { return c.m_rxPackets; }
    static quint64 txBytes(const P2Client& c) { return c.m_txBytes; }
};
}  // namespace AetherSDR::anan

static P2Client::Params loopbackParams()
{
    P2Client::Params p;
    p.host = QStringLiteral("127.0.0.1");
    p.speakerAudioEnabled = false;
    return p;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // QSignalSpy records the argument by metatype. Registered explicitly so a
    // capture failure can never present as an empty-but-passing assertion.
    qRegisterMetaType<P2Client::LinkCounters>();

    // ---- a fresh client has measured nothing ----
    {
        P2Client client;
        check(P2ClientTestAccess::rxBytes(client) == 0
                  && P2ClientTestAccess::rxPackets(client) == 0
                  && P2ClientTestAccess::txBytes(client) == 0,
              "a client that has not run has all counters at zero");
    }

    // ---- every datagram counts, including ones the parsers reject ----
    {
        P2Client client;

        // A High Priority Status packet: 60 bytes, not DDC-shaped, so
        // handleDatagram() returns without ever reaching the IQ path.
        std::vector<std::uint8_t> status(kHighPriorityStatusBytes, 0);
        status[30] = 0b0000'1000;
        P2ClientTestAccess::feedDatagram(client, status, 1025);
        check(P2ClientTestAccess::rxPackets(client) == 1
                  && P2ClientTestAccess::rxBytes(client) == kHighPriorityStatusBytes,
              "a Status packet counts: it is transport traffic, not IQ");

        // Garbage too short to be any known packet. THIS is the case that would
        // vanish if counting happened after the shape checks instead of before.
        const std::vector<std::uint8_t> runt{0x01, 0x02, 0x03};
        P2ClientTestAccess::feedDatagram(client, runt, 1025);
        check(P2ClientTestAccess::rxPackets(client) == 2
                  && P2ClientTestAccess::rxBytes(client)
                         == kHighPriorityStatusBytes + runt.size(),
              "a datagram every parser rejects still counts as traffic arrived");

        // DDC-shaped but from a port this session never enabled: dropped WITHOUT
        // counting a sequence drop, yet the bytes did cross the wire.
        std::vector<std::uint8_t> ddcish(kDdcHeaderLen + kDdcSampleBytes, 0);
        ddcish[14] = 0x00; ddcish[15] = 0x01;   // one sample declared
        const quint64 beforeStray = P2ClientTestAccess::rxBytes(client);
        P2ClientTestAccess::feedDatagram(client, ddcish, 9999);
        check(P2ClientTestAccess::rxBytes(client) > beforeStray
                  && P2ClientTestAccess::rxPackets(client) == 3,
              "a datagram from an unexpected sender port counts as traffic too");

        check(client.droppedPackets() == 0,
              "...and none of those is a sequence drop -- the two counters are"
              " independent");
    }

    // ---- the snapshot carries the counters, and is what crosses threads ----
    {
        P2Client client;
        std::vector<std::uint8_t> status(kHighPriorityStatusBytes, 0);
        P2ClientTestAccess::feedDatagram(client, status, 1025);

        QSignalSpy snapshots(&client, &P2Client::linkCountersUpdated);
        P2ClientTestAccess::publish(client);
        check(snapshots.count() == 1, "publishing emits exactly one snapshot");
        if (snapshots.count() == 1) {
            const auto counters =
                snapshots.at(0).at(0).value<P2Client::LinkCounters>();
            check(counters.rxPackets == 1
                      && counters.rxBytes == kHighPriorityStatusBytes,
                  "the snapshot carries the receive counters as the client has"
                  " them");
            check(counters.drops == 0, "and the drop total alongside them");
        }
    }

    // ---- sent bytes are counted, and start() resets the session ----
    {
        P2Client client;

        // Traffic from a PREVIOUS notional session, which must not survive.
        std::vector<std::uint8_t> status(kHighPriorityStatusBytes, 0);
        P2ClientTestAccess::feedDatagram(client, status, 1025);
        check(P2ClientTestAccess::rxBytes(client) > 0, "pre-start traffic counted");

        if (!client.start(loopbackParams())) {
            std::fprintf(stderr,
                         "anan_link_telemetry_test: SKIP -- client could not bind"
                         " a local UDP socket\n");
            return 77;   // SKIP_RETURN_CODE in tests.cmake
        }

        check(P2ClientTestAccess::rxBytes(client) == 0
                  && P2ClientTestAccess::rxPackets(client) == 0,
              "start() resets the receive counters, so a reconnect does not"
              " inherit the last session's totals");

        // start() sends its own startup sequence (Discovery, General,
        // DDC-Specific, High Priority) before returning.
        check(P2ClientTestAccess::txBytes(client) > 0,
              "the startup sequence is counted as sent bytes");

        const QString endpoint = [&] {
            QSignalSpy snapshots(&client, &P2Client::linkCountersUpdated);
            P2ClientTestAccess::publish(client);
            return snapshots.count() == 1
                       ? snapshots.at(0).at(0).value<P2Client::LinkCounters>()
                             .localEndpoint
                       : QString();
        }();
        check(endpoint.contains(QLatin1Char(':')),
              "a running client reports its bound local endpoint as \"ip:port\"");

        client.stop();
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_link_telemetry_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
