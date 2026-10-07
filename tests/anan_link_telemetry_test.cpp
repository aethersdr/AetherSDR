// aetherd ANAN P2 -- transport counters and the LinkStats snapshot.
//
// The status bar's "Network:" field was blank on an ANAN because the backend
// published no LinkStats at all. The counters behind it live on P2Client, are
// written only from its own I/O thread, and reach the backend as a periodic
// snapshot rather than a cross-thread read. What this pins:
//
//   * Every byte from the radio counts, including datagrams every parser
//     rejects, so a session whose IQ has stalled while Status packets still
//     arrive does not read as a dead link.
//   * Packets count DDC frames only: they are the denominator of the loss
//     percentage, whose numerator is DDC sequence gaps.
//   * A datagram from any address but the radio's counts toward nothing.
//   * Sent bytes are counted through sendTo(), which every send but the speaker
//     stream uses. The speaker path counts its own and is not pinned here.
//   * start() resets the session, so a reconnect does not inherit totals.
//
// The last block BINDS A UDP SOCKET (AnyIPv4, ephemeral port) and sends the
// startup sequence to 127.0.0.1; nothing listens and no radio is involved. It
// skips (exit 77) when it cannot bind, unless an earlier check already failed.

#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/P2Protocol.h"

#include <QCoreApplication>
#include <QHostAddress>
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
    // The socket's half, minus the socket: the sender address is what onReadyRead()
    // reads off the QNetworkDatagram.
    static void feedFrom(P2Client& c, const QHostAddress& sender,
                         std::span<const std::uint8_t> bytes, quint16 port)
    {
        c.handleDatagramFrom(sender, bytes, port);
    }
    static void setRadioHost(P2Client& c, const QHostAddress& host) { c.m_host = host; }
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

// A DDC I&Q frame parseDdcFrame() accepts: 24 bits per sample, one sample,
// length exactly header + one sample.
static std::vector<std::uint8_t> ddcFrame(std::uint32_t seq)
{
    std::vector<std::uint8_t> f(kDdcHeaderLen + kDdcSampleBytes, 0);
    f[0] = static_cast<std::uint8_t>(seq >> 24);
    f[1] = static_cast<std::uint8_t>(seq >> 16);
    f[2] = static_cast<std::uint8_t>(seq >> 8);
    f[3] = static_cast<std::uint8_t>(seq);
    f[13] = 24;   // bits per sample
    f[15] = 1;    // samples in this frame
    return f;
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

    // ---- bytes count every datagram; packets count DDC frames only ----
    {
        P2Client client;

        // A High Priority Status packet: 60 bytes, not DDC-shaped, so
        // handleDatagram() returns without ever reaching the IQ path.
        std::vector<std::uint8_t> status(kHighPriorityStatusBytes, 0);
        status[30] = 0b0000'1000;
        P2ClientTestAccess::feedDatagram(client, status, 1025);
        check(P2ClientTestAccess::rxBytes(client) == kHighPriorityStatusBytes,
              "a Status packet's bytes count: it is transport traffic");
        check(P2ClientTestAccess::rxPackets(client) == 0,
              "...but it is not a packet of the stream drops are counted on");

        // Garbage too short to be any known packet. THIS is the case that would
        // vanish if bytes were counted after the shape checks instead of before.
        const std::vector<std::uint8_t> runt{0x01, 0x02, 0x03};
        P2ClientTestAccess::feedDatagram(client, runt, 1025);
        check(P2ClientTestAccess::rxBytes(client)
                  == kHighPriorityStatusBytes + runt.size(),
              "a datagram every parser rejects still counts as bytes arrived");

        // A real DDC frame, but from a port this session never enabled: dropped
        // WITHOUT counting a sequence drop, so it must not count as a packet
        // either, yet its bytes did cross the wire.
        const auto stray = ddcFrame(7);
        const quint64 beforeStray = P2ClientTestAccess::rxBytes(client);
        P2ClientTestAccess::feedDatagram(client, stray, 9999);
        check(P2ClientTestAccess::rxBytes(client) == beforeStray + stray.size(),
              "a DDC frame from an unexpected sender port counts as bytes");
        check(P2ClientTestAccess::rxPackets(client) == 0,
              "...and not as a packet: drops are never counted for that port");

        // The positive case: DDC0's own port.
        const auto ddc0 = ddcFrame(1);
        P2ClientTestAccess::feedDatagram(client, ddc0, kDdc0DefaultPort);
        check(P2ClientTestAccess::rxPackets(client) == 1,
              "a DDC0 frame from the session's own port is one packet");

        check(client.droppedPackets() == 0,
              "...and none of those is a sequence drop -- the two counters are"
              " independent");
    }

    // ---- only the radio's own address counts ----
    {
        P2Client client;
        const QHostAddress radio(QStringLiteral("192.0.2.10"));
        P2ClientTestAccess::setRadioHost(client, radio);
        const auto frame = ddcFrame(1);

        P2ClientTestAccess::feedFrom(client, QHostAddress(QStringLiteral("192.0.2.99")),
                                     frame, kDdc0DefaultPort);
        check(P2ClientTestAccess::rxBytes(client) == 0
                  && P2ClientTestAccess::rxPackets(client) == 0,
              "a DDC frame from another LAN host counts toward nothing, so it"
              " cannot hold the link alive after the radio has gone");

        P2ClientTestAccess::feedFrom(client, radio, frame, kDdc0DefaultPort);
        check(P2ClientTestAccess::rxBytes(client) == frame.size()
                  && P2ClientTestAccess::rxPackets(client) == 1,
              "the same frame from the radio's address counts");

        // The socket binds AnyIPv4, but a dual-stack bind would report the radio
        // in v4-mapped form; the comparison must not drop it then either.
        P2ClientTestAccess::feedFrom(client,
                                     QHostAddress(QStringLiteral("::ffff:192.0.2.10")),
                                     ddcFrame(2), kDdc0DefaultPort);
        check(P2ClientTestAccess::rxPackets(client) == 2,
              "the radio's address in v4-mapped form is still the radio");
    }

    // ---- the snapshot carries the counters, and is what crosses threads ----
    {
        P2Client client;
        std::vector<std::uint8_t> status(kHighPriorityStatusBytes, 0);
        P2ClientTestAccess::feedDatagram(client, status, 1025);
        const auto ddc0 = ddcFrame(1);
        P2ClientTestAccess::feedDatagram(client, ddc0, kDdc0DefaultPort);

        QSignalSpy snapshots(&client, &P2Client::linkCountersUpdated);
        P2ClientTestAccess::publish(client);
        check(snapshots.count() == 1, "publishing emits exactly one snapshot");
        if (snapshots.count() == 1) {
            const auto counters =
                snapshots.at(0).at(0).value<P2Client::LinkCounters>();
            check(counters.rxPackets == 1
                      && counters.rxBytes == kHighPriorityStatusBytes + ddc0.size(),
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
        P2ClientTestAccess::feedDatagram(client, ddcFrame(1), kDdc0DefaultPort);
        check(P2ClientTestAccess::rxBytes(client) > 0
                  && P2ClientTestAccess::rxPackets(client) > 0,
              "pre-start traffic counted");

        if (!client.start(loopbackParams())) {
            std::fprintf(stderr,
                         "anan_link_telemetry_test: SKIP -- client could not bind"
                         " a local UDP socket\n");
            // Exit 77 (SKIP_RETURN_CODE) reads green, so it is only a skip
            // while nothing above has failed.
            return g_failures == 0 ? 77 : 1;
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
