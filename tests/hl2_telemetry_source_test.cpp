// telemetrySource must tell TWO states apart, and the merge must not erase
// readings. Both were broken; neither was caught by a passing test.
//
// THE DEFECT. Hl2TelemetryService took over the four telemetry rows and
// Hl2Backend stopped publishing `telemetrySource`, so the service's value always
// won the merge and `in-band` became unreachable. Observed on hardware during
// Config C: the app connected, EP6 healthy, the row reading `port-1025`. The
// row exists to say which path produced the numbers, and it could only ever
// name one of the two.
//
// WHY THIS TEST IS A TRUTH TABLE AND NOT A SCENARIO. A single case proves
// nothing here: a test that only checked the disconnected case passes on the
// broken code, and one that only checked the connected case would have passed
// on the ORIGINAL code before the service existed. The row's whole purpose is
// discrimination, so the test has to see both answers — and the cheapest honest
// way to see all of them is to enumerate the inputs.
//
// WHY IT IS NOT DRIVEN THROUGH Hl2Backend. It cannot be, without a radio or a
// simulator: the inputs are m_connected and m_telemetry, both private and both
// set only by the wire. hl2_telemetry_wire_test drives the backend because the
// thing it checks — that a timer fires — is observable from outside; this one
// is not. So the decision lives in a pure function that PRODUCTION CALLS, which
// is the same trade Hl2TxLevelPolicy.h documents: a test against a re-typed
// copy of a mapping proves only that two copies agree.

#include "core/backends/hl2/Hl2TelemetrySource.h"

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

int main()
{
    // ---- the discriminator, both answers, in one test ----
    check(hl2TelemetrySource(/*connected=*/true, /*inBand=*/true, /*streamFree=*/false)
              == kTelemetrySourceInBand,
          "connected and streaming -> 'in-band'");
    check(hl2TelemetrySource(false, false, true) == kTelemetrySourcePort1025,
          "disconnected with a stream-free reading -> 'port-1025'");

    // In-band wins when BOTH have something: it is fresher (10 Hz against
    // 1-2 Hz) and its cadence is ours. This is the case the live run was in
    // when the row said port-1025.
    check(hl2TelemetrySource(true, true, true) == kTelemetrySourceInBand,
          "both present while connected -> 'in-band' wins");

    // ---- connected is REQUIRED for in-band, not merely correlated ----
    // Hl2Telemetry's EP6 readings persist after a session ends, so a
    // disconnected app still holding them must not claim in-band. Reporting
    // stale values as live is the frozen-reading failure this whole feature was
    // built to expose.
    check(hl2TelemetrySource(false, true, false) == kTelemetrySourceNone,
          "disconnected with STALE in-band values -> 'none', never 'in-band'");
    check(hl2TelemetrySource(false, true, true) == kTelemetrySourcePort1025,
          "disconnected with stale in-band AND a fresh poll -> the poll wins");

    // ---- nothing at all is a claim, not a blank ----
    check(hl2TelemetrySource(false, false, false) == kTelemetrySourceNone,
          "nothing anywhere -> 'none'");
    check(hl2TelemetrySource(true, false, false) == kTelemetrySourceNone,
          "connected but nothing reported yet -> 'none', not 'in-band'");
    check(hl2TelemetrySource(true, false, true) == kTelemetrySourcePort1025,
          "connected, in-band silent, poll answered -> 'port-1025' (the stalled case)");

    // ---- the merge, and the rule that silently eats readings ----
    {
        AetherSDR::IRadioBackend::HealthSnapshot base;      // the service: stream-free
        base.order = {QStringLiteral("temperatureRaw"), QStringLiteral("telemetrySource")};
        base.labels.insert(QStringLiteral("temperatureRaw"), QStringLiteral("Temp"));
        base.labels.insert(QStringLiteral("telemetrySource"), QStringLiteral("Source"));
        base.values.insert(QStringLiteral("temperatureRaw"), 1028);
        base.values.insert(QStringLiteral("telemetrySource"), kTelemetrySourcePort1025);

        AetherSDR::IRadioBackend::HealthSnapshot winner;    // the backend: in-band
        winner.order = {QStringLiteral("telemetrySource"), QStringLiteral("firmwareVersion"),
                        QStringLiteral("temperatureRaw")};
        winner.labels.insert(QStringLiteral("telemetrySource"), QStringLiteral("Source"));
        winner.labels.insert(QStringLiteral("firmwareVersion"), QStringLiteral("Firmware"));
        winner.labels.insert(QStringLiteral("temperatureRaw"), QStringLiteral("Temp"));
        winner.values.insert(QStringLiteral("telemetrySource"), kTelemetrySourceInBand);
        winner.values.insert(QStringLiteral("firmwareVersion"), 0x4A);
        // temperatureRaw is DECLARED but has NO value: "the radio never
        // reported this". It must not erase the stream-free reading below.

        const auto m = hl2MergeHealth(base, winner);

        check(m.values.value(QStringLiteral("telemetrySource")).toString()
                  == kTelemetrySourceInBand,
              "merge: the backend's source row WINS — this is the whole defect");
        check(m.values.value(QStringLiteral("temperatureRaw")).toInt() == 1028,
              "merge: a key the winner declares but does not VALUE must not erase "
              "the base's reading — overwriting with nothing is how a number "
              "becomes a dash");
        check(m.values.value(QStringLiteral("firmwareVersion")).toInt() == 0x4A,
              "merge: the winner's own new rows arrive");
        check(m.order.contains(QStringLiteral("firmwareVersion")),
              "merge: a row only the winner has is ordered in");
        check(m.order.count(QStringLiteral("telemetrySource")) == 1,
              "merge: a shared row is not duplicated in the order");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_source_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
