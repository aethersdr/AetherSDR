// Does anything ASK the cadence rule? Driven through Hl2Backend, on purpose.
//
// hl2_telemetry_cadence_test proves the rule is right. hl2_telemetry_service_test
// proves the service polls and reports. Neither can fail when the BACKEND stops
// telling the service what the IQ path is doing — and that is exactly what
// happened, twice, in two different ways:
//
//   1. The poll state was first driven from publishLinkStats(), whose timer
//      STOPS on linkDown and connectFailed — so the poller went unmanaged in the
//      three states it exists for. Caught by reading, before it shipped.
//   2. The 1 Hz tick added to fix (1) was then DELETED by the service refactor:
//      a regex removing the backend's poller construction swallowed the timer
//      beside it. Nothing failed. The rule stayed correct, its unit test stayed
//      green, and a live connect showed `connected=True pollMs=1000` — the
//      poller still polling 1025 through a healthy stream, which is the precise
//      thing the cadence exists to prevent.
//
// Four times in one session a correct rule sat behind a passing test with
// nothing asking it. So this test does not ask whether the rule is right. It
// asks whether the WIRE is there: construct a real Hl2Backend, give it a real
// service, let the event loop run, and require the backend to have pushed link
// state in of its own accord.
//
// No radio, no connection, no network: the assertion is about the driver
// existing, and a driver that only runs while connected is the first bug again.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2TelemetryService.h"

#include <QCoreApplication>
#include <QString>
#include <QEventLoop>
#include <QTimer>
#include <QVariant>

#include <cstdio>

using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::Hl2LinkState;
using AetherSDR::hl2::Hl2TelemetryService;
using AetherSDR::hl2::hl2PollIntervalMs;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2TelemetryService svc;
    Hl2Backend backend;
    backend.setTelemetryService(&svc);

    check(svc.linkStateUpdateCount() == 0,
          "nothing has driven the service before the event loop runs");

    // Long enough for several ticks of a ~1 Hz driver. Not connected, no radio,
    // no target: a backend that only drives its telemetry while a session is up
    // is the first version of this bug, so this must hold with nothing
    // connected at all.
    spin(2600);

    const int updates = svc.linkStateUpdateCount();
    check(updates >= 2,
          "the backend drives the service PERIODICALLY, with no connection and "
          "no radio — this is the wire that was deleted");
    if (updates < 2)
        std::fprintf(stderr, "  (linkStateUpdateCount = %d after 2.6 s)\n", updates);

    // And the rule it feeds still says what it should, so a green wire test
    // cannot be read as approval of a broken table.
    check(hl2PollIntervalMs(Hl2LinkState::Streaming, true) == 0,
          "and the rule the wire feeds still returns 0 for Streaming");

    // ---- the backend PUBLISHES an attribution row at all ----
    //
    // This is the second half of the same class of defect, and it is red on the
    // tree as it stands. When Hl2TelemetryService took the four telemetry rows
    // over, Hl2Backend stopped emitting `telemetrySource` — so the service's
    // value always won the merge and `in-band` became unreachable. Observed on
    // hardware during Config C: connected, EP6 healthy, the row reading
    // `port-1025`.
    //
    // Asserted HERE rather than in the pure-policy test because the policy
    // function cannot fail this way: the bug is not a wrong answer, it is a row
    // that is never asked for. Only the real snapshot can show its absence.
    const auto snap = backend.healthSnapshot();
    check(snap.order.contains(QStringLiteral("telemetrySource")),
          "the BACKEND publishes a telemetrySource row — without it the service "
          "always wins the merge and 'in-band' is unreachable");

    // ---- and it does not publish it as a CONSTANT ----
    //
    // Publishing the row was only half the repair. The backend passed
    // haveStreamFree=false literally, and because this row WINS the merge that
    // made `port-1025` unreachable for as long as a backend existed — the
    // stalled case the design note calls the one that matters most. With no
    // connection and nothing having answered the poller, the honest answer is
    // `none`; it was `none` before the fix too, so this check is a floor and
    // the discriminating case is the one below it.
    check(snap.values.value(QStringLiteral("telemetrySource")).toString()
              == QStringLiteral("none"),
          "disconnected, nothing polled -> telemetrySource is 'none'");

    // ---- the in-band READINGS do not win the merge when in-band is silent ----
    //
    // m_telemetry accumulates from EP6 and is never cleared, so every field's
    // last value simply stays; "in-band wins on key collision" therefore meant
    // pre-stall readings beating fresh port-1025 ones for the life of the
    // process. The rows must report nothing whenever the link is not Streaming.
    //
    // `ptt` is the one that discriminates a regression fastest: it is a plain
    // bool with a default of false, so before the fix a disconnected backend
    // published `ptt = false` — a positive claim that the radio is unkeyed,
    // which WINS the merge and overwrites whatever the poller actually read
    // from the radio.
    for (const char* key : {"ptt", "temperatureRaw", "forwardPowerRaw",
                            "reversePowerRaw", "biasCurrentRaw", "txFifoFillMsbs"}) {
        const QString k = QString::fromLatin1(key);
        check(snap.order.contains(k) && !snap.values.contains(k),
              qPrintable(QStringLiteral("in-band '%1' is published as 'not reported' "
                                        "while the stream is not delivering, so the "
                                        "stream-free reading survives the merge").arg(k)));
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_wire_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
