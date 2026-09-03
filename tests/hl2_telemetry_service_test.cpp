// The stream-free telemetry service must answer with NO backend and NO
// connection. Roadmap #15; this is the test the feature should have had first.
//
// WHY IT EXISTS. The poller originally lived inside Hl2Backend. Everything
// passed — the cadence rule's unit test, the protocol test, and a check that 32
// Hl2TelemetryPoller symbols were linked into the shipped binary. All of it was
// true and none of it asked the only question that mattered: does anything
// CONSTRUCT the poller in the state the feature exists for?
//
// It did not. `RadioModel::backendHealthSnapshot()` is
// `m_backend ? m_backend->healthSnapshot() : HealthSnapshot{}` and m_backend is
// built inside connectToRadio(), so a disconnected app has no backend, no
// poller, and an empty health snapshot. Two prechecks against a real launched
// app confirmed it: `total rows in snapshot: 0`, twice, for 14 s and 22 s.
//
// The rule that came out of it, and what this test defends:
//
//     AN INSTRUMENT FOR THE NO-CONNECTION CASE MUST NOT BE OWNED BY THE
//     CONNECTION.
//
// So this test constructs the service alone — no RadioModel, no backend, no
// connection, nothing but a Qt event loop — and requires it to answer.
//
// THE TARGET IS 192.0.2.1: TEST-NET-1, unroutable by RFC 5737. The poller will
// send to it and nothing will ever reply, which is the point — it exercises the
// "asked and heard nothing" path without a radio, without a network peer, and
// without any possibility of reaching real hardware. Radio-silent by
// construction rather than by intention, which is what lets this run at any
// time regardless of who holds the bench.

#include "core/backends/hl2/Hl2TelemetryService.h"

#include <QCoreApplication>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Spin the event loop for ms without blocking timers, so the poller's own
// QTimer actually fires. A sleep would freeze the thing under test.
static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static QVariant rowValue(const AetherSDR::IRadioBackend::HealthSnapshot& s, const char* key)
{
    const auto it = s.values.constFind(QString::fromLatin1(key));
    return it != s.values.constEnd() ? *it : QVariant();
}

static bool hasRow(const AetherSDR::IRadioBackend::HealthSnapshot& s, const char* key)
{
    return s.order.contains(QString::fromLatin1(key));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2TelemetryService svc;                       // no backend, no connection

    // Before anything is asked for, the service must not be polling: nothing is
    // reading, so nothing should be on the wire.
    svc.setTarget(QHostAddress(QStringLiteral("192.0.2.1")));
    svc.setLinkState(Hl2LinkState::NotConnected);

    // Reading the snapshot is the demand signal. This is the call a disconnected
    // app makes through the bridge's `health` verb.
    svc.noteDemand();
    auto snap = svc.healthRows();

    // ---- 1. The rows exist AT ALL. This is the whole bug. ----
    check(!snap.order.isEmpty(),
          "a disconnected service answers with rows, not an empty snapshot");
    check(hasRow(snap, "telemetrySource"),   "telemetrySource row exists with no backend");
    check(hasRow(snap, "telemetryAgeMs"),    "telemetryAgeMs row exists with no backend");
    check(hasRow(snap, "telemetryUnanswered"), "telemetryUnanswered row exists with no backend");
    check(hasRow(snap, "telemetryPollMs"),   "telemetryPollMs row exists with no backend");

    // ---- 2. With nothing answering, the source is `none` — not a blank ----
    // `none` and "absent" are different claims. A blank would let a reader
    // believe the field is unsupported; `none` says we looked and nobody spoke.
    check(rowValue(snap, "telemetrySource").toString() == QStringLiteral("none"),
          "no reply yet -> telemetrySource is 'none', not empty");

    // ---- 3. Demand turns the poller on, and the interval is visible ----
    check(rowValue(snap, "telemetryPollMs").toInt()
              == hl2PollIntervalMs(Hl2LinkState::NotConnected, /*surfaceVisible=*/true),
          "poll interval matches the cadence rule for a watched idle radio");

    // ---- 4. Unanswered polls COUNT. This is the third state. ----
    // `null` already means "the radio never reported this field". Without a
    // count, "we asked and heard nothing" is indistinguishable from "we never
    // asked" — one is a fault to chase, the other is the poller being correctly
    // idle, and they want opposite responses.
    //
    // 192.0.2.1 can never answer, so after several intervals this must be
    // non-zero. Keep asking, because demand decays.
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

    // ---- 5. And still no reading, honestly reported ----
    snap = svc.healthRows();
    check(rowValue(snap, "telemetrySource").toString() == QStringLiteral("none"),
          "still 'none' after unanswered polls — an unanswered poll is not a reading");
    check(!svc.lastReply().has_value(),
          "lastReply stays absent — never a default-constructed reply standing in for one");
    check(!rowValue(snap, "telemetryAgeMs").isValid(),
          "age is ABSENT with no reply, not 0 — zero would read as 'fresh'");

    // ---- 6. WHERE the packets go, observed on a real socket ----
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
        QUdpSocket listener;
        const bool bound = listener.bind(QHostAddress::LocalHost, 1025,
                                         QUdpSocket::ShareAddress);
        check(bound, "test listener bound on 127.0.0.1:1025");

        if (bound) {
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
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_service_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
