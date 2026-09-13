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
// NOTHING HERE TOUCHES THE WIRE, and that is a property of the code rather
// than of care taken while writing it: the service is never given a target,
// and with no target and the broadcast fallback off by default the poller
// sends nothing at all. No socket is bound, no datagram is sent, no peer
// exists. The cases that DO need a wire — an unanswered poll being counted,
// and a named target actually receiving the EF FE 02 request — live in
// tests/hl2_telemetry_wire_socket_test.cpp, behind an explicit opt-in and off
// the default graph (AGENTS.md's test-layer boundary).

#include "core/backends/hl2/Hl2TelemetryService.h"

#include <QCoreApplication>
#include <QVariant>

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

    // ---- 3. NO TARGET IS NOT POLLING, and the row must say so ----
    //
    // This assertion used to read the other way: it required the row to equal
    // hl2PollIntervalMs(NotConnected, true) = 1000, with no target set and
    // therefore nothing whatever on the wire. It passed, because the poller
    // reported the cadence rule's answer while onPollTimer() separately
    // returned early for want of a destination — two halves that never
    // compared notes. So `health` read "polling every 1000 ms, 0 unanswered"
    // about a silent socket, and the test agreed with it.
    //
    // The row's own legend is "0 = not polling". Nowhere to send is not
    // polling, whatever the cadence rule would say about the link state, and
    // pinning the cadence rule here was pinning the wrong function: the rule is
    // already pinned, without a socket, by hl2_telemetry_cadence_test.
    //
    // The positive case — a NAMED target actually producing the cadence rule's
    // interval — needs a socket to be honest about, so it lives in the opt-in
    // tests/hl2_telemetry_wire_socket_test.cpp rather than here.
    check(rowValue(snap, "telemetryPollMs").toInt() == 0,
          "no target and no broadcast fallback -> the poll interval is 0, "
          "because 0 is what this row means by 'not polling'");

    // ---- 4. Absent is not zero, and absent is not a reading ----
    // `null` means "the radio never reported this". Zero would read as "fresh",
    // and a default-constructed reply would read as "the radio answered with
    // zeros" — a measurement that never happened.
    check(!svc.lastReply().has_value(),
          "lastReply stays absent — never a default-constructed reply standing in for one");
    check(!rowValue(snap, "telemetryAgeMs").isValid(),
          "age is ABSENT with no reply, not 0 — zero would read as 'fresh'");
    // Three states, not two. A count of 0 printed while nothing is being asked
    // reads as "we are asking and all is well"; absent says we are not asking.
    check(!rowValue(snap, "telemetryUnanswered").isValid(),
          "not polling -> unanswered is ABSENT, not 0 — 0 would read as 'asking, all fine'");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_service_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
