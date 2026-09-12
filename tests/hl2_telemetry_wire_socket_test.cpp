// The stream-free HL2 poller's WIRE behaviour: where the datagrams go, and
// what an unanswered poll is counted as.
//
// OFF THE DEFAULT CTEST GRAPH ON PURPOSE. This binds a real UDP socket and
// sends real datagrams, which AGENTS.md's test-layer boundary keeps out of the
// ordinary suite. It is registered only under
// AETHER_ENABLE_HL2_TELEMETRY_SOCKET_TEST, and it EXITS 77 (Skipped) rather
// than failing when the socket cannot be taken — a port another process owns is
// not evidence about this poller.
//
// SOCKETS THIS TEST BINDS, per the socket-test canon: one IPv4 UDP listener on
// 127.0.0.1:1025 (ShareAddress), which is the Metis discovery port, and it
// receives only what this test's own service instances send to it. Outbound: to
// 127.0.0.1 (that listener) and to 192.0.2.1 — TEST-NET-1, unroutable by RFC
// 5737, which is what makes the unanswered-poll path reachable without a radio
// and without any possibility of touching real hardware. No listening server,
// no peer process, no synthetic radio firmware: the code under test is ours and
// the socket is how the assertion reaches it.
//
// The claims that need no wire — the rows existing with no backend,
// telemetrySource being `none`, an absent age — live in
// tests/hl2_telemetry_service_test.cpp and run by default.

#include "core/backends/hl2/Hl2TelemetryService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>

#include <cstdint>
#include <cstdio>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Spin the event loop for ms without blocking timers, so the poller's own
// QTimer actually fires. A sleep would freeze the thing under test.
void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // FAIL FAST AS A SKIP. Take the socket first, before any assertion depends
    // on it: if this port belongs to something else on this machine, that says
    // nothing about the poller and must not read as a failed proof.
    QUdpSocket listener;
    if (!listener.bind(QHostAddress::LocalHost, 1025, QUdpSocket::ShareAddress)) {
        std::fprintf(stderr,
                     "SKIP: could not bind 127.0.0.1:1025 (%s)\n",
                     qPrintable(listener.errorString()));
        return 77;
    }

    // ---- 1. Unanswered polls COUNT. This is the third state. ----
    // `null` already means "the radio never reported this field". Without a
    // count, "we asked and heard nothing" is indistinguishable from "we never
    // asked" — one is a fault to chase, the other is the poller being correctly
    // idle, and they want opposite responses.
    //
    // 192.0.2.1 can never answer, so after several intervals this must be
    // non-zero. Keep asking, because demand decays.
    {
        Hl2TelemetryService svc;
        svc.setTarget(QHostAddress(QStringLiteral("192.0.2.1")));
        svc.setLinkState(Hl2LinkState::NotConnected);

        // The POSITIVE half of "0 = not polling", which needs a socket to be
        // honest about and so cannot live in the socket-free service test: with
        // a target named, the reported interval is the cadence rule's answer.
        // Its negative half — no target, interval 0 — is pinned there.
        svc.noteDemand();
        check(svc.healthRows().values.value(QStringLiteral("telemetryPollMs")).toInt()
                  == hl2PollIntervalMs(Hl2LinkState::NotConnected, /*surfaceVisible=*/true),
              "a NAMED target reports the cadence rule's interval, not 0");

        QElapsedTimer waited;
        waited.start();
        int unanswered = 0;
        while (waited.elapsed() < 6000) {
            spin(500);
            svc.noteDemand();
            unanswered = svc.healthRows().values
                             .value(QStringLiteral("telemetryUnanswered")).toInt();
            if (unanswered > 0)
                break;
        }
        check(unanswered > 0,
              "polls to an unroutable target are counted as unanswered, not silently dropped");

        // And still no reading, honestly reported: an unanswered poll is not a
        // measurement, however many of them there are.
        const auto snap = svc.healthRows();
        check(snap.values.value(QStringLiteral("telemetrySource")).toString()
                  == QStringLiteral("none"),
              "still 'none' after unanswered polls — an unanswered poll is not a reading");
        check(!svc.lastReply().has_value(),
              "lastReply stays absent after unanswered polls");
    }

    // ---- 2. WHERE the packets go, observed on a real socket ----
    //
    // Not "does it think it polled" but "did a datagram arrive at the address
    // we named, and only there". This is the check that would have caught the
    // bench's actual topology problem: the poller's broadcast fallback sends to
    // the LOCAL SEGMENT, which here is the segment the ka9q station receiver
    // sits on, while the radio under test is off-net behind a gateway and can
    // never receive a broadcast at all. Inverted in both directions -- unable
    // to reach the intended host, able to reach one that must not be polled.
    //
    // So the default is now: no target, no packets. A caller names the radio.
    {
        // (a) NO TARGET -> NOTHING ON THE WIRE. The old default would have
        // broadcast here.
        Hl2TelemetryService silent;
        silent.setLinkState(Hl2LinkState::NotConnected);
        for (int i = 0; i < 6; ++i) { silent.noteDemand(); spin(500); }
        check(!listener.hasPendingDatagrams(),
              "no target: the poller sends NOTHING, it does not broadcast");

        // (b) TARGET NAMED -> a unicast poll arrives, at that address, and
        // it is the EF FE 02 status request and nothing else.
        Hl2TelemetryService aimed;
        aimed.setTarget(QHostAddress::LocalHost);
        aimed.setLinkState(Hl2LinkState::NotConnected);
        bool sawRequest = false;
        bool wrongSender = false;
        for (int i = 0; i < 8 && !sawRequest; ++i) {
            aimed.noteDemand();
            spin(500);
            while (listener.hasPendingDatagrams()) {
                const QNetworkDatagram dg = listener.receiveDatagram();
                const QByteArray d = dg.data();
                if (d.size() >= 3 && std::uint8_t(d[0]) == 0xEF
                    && std::uint8_t(d[1]) == 0xFE && std::uint8_t(d[2]) == 0x02)
                    sawRequest = true;
                else
                    wrongSender = true;
            }
        }
        check(sawRequest,
              "target named: a unicast EF FE 02 arrives AT THAT ADDRESS");
        check(!wrongSender,
              "and nothing else is sent -- only the read-only status request");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_wire_socket_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
