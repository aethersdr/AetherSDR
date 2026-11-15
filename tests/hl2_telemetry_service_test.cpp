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

    // ---- 5. A section is a GROUP HEADING, not a tag on every row ----
    //
    // docs/automation-bridge.md: "section appears on the first row of each
    // group and is absent on the rest". RadioHealthDialog draws a bold header
    // for every key that carries one, so stamping them all drew eleven repeated
    // headers interleaved with the rows. ten9876, #5642.
    {
        int withSection = 0;
        for (const QString& k : snap.order)
            if (snap.sections.contains(k))
                ++withSection;
        std::fprintf(stderr, "  sections: %d of %lld rows carry one\n",
                     withSection, static_cast<long long>(snap.order.size()));
        check(snap.order.size() > 1, "there is more than one row to group");
        check(withSection == 1,
              "exactly one row carries the section — the group's first");
        check(!snap.order.isEmpty() && snap.sections.contains(snap.order.first()),
              "and it is the FIRST row, not an arbitrary one");
    }

    // ---- 6. radioInUse may not report OUR OWN stream as another client ----
    //
    // The `run` bit in a discovery reply says somebody is streaming; it does
    // not say who. While connected and stalled the poller still runs and that
    // somebody is us, so publishing the bit told the operator another client
    // held the radio during their own stalled session — in exactly the state
    // this feature exists to diagnose. Hl2Backend::healthSnapshot() publishes
    // no radioInUse key, so nothing corrected it at the merge. ten9876, #5642.
    {
        DiscoveryReply r;
        r.streaming = true;

        Hl2TelemetryService owned;
        Hl2TelemetryServiceTestAccess::placeReply(owned, r);

        owned.setLinkState(Hl2LinkState::HeldByOther);
        auto held = owned.healthRows();
        check(rowValue(held, "radioInUse").isValid()
                  && rowValue(held, "radioInUse").toBool(),
              "held by another client -> the bit is published, and it is true");

        // setLinkState(Streaming) does not clear the cache: leftOurOwnSession
        // fires on LEAVING a streaming state, not on entering one.
        owned.setLinkState(Hl2LinkState::StreamStalled);
        auto stalled = owned.healthRows();
        check(!owned.lastReply().has_value()
                  || !rowValue(stalled, "radioInUse").isValid(),
              "our own stalled session -> radioInUse is ABSENT, because the bit "
              "cannot tell another client from us");
        check(rowValue(stalled, "radioInUse").toBool() == false,
              "and it is certainly never TRUE about our own stream");
    }

    // ---- 7. the stream-free path answers the PA temperature IN DEGREES ----
    //
    // Hl2Backend gates its own `temperatureC` on the in-band path actually
    // delivering, which is right. But nothing took the row over, so the three
    // states this class exists for rendered "PA temperature (°C): —" beside a
    // four-digit raw count, and "what is its PA temperature while somebody else
    // has the stream" -- one of the three questions the feature opens with --
    // went unanswered (#5642 review).
    //
    // ONE FORMULA, shared: Hl2Backend::temperatureCelsius() forwards to the
    // same hl2TemperatureCelsius() this row uses, so the two paths cannot
    // report different degrees for the same count.
    {
        DiscoveryReply r;
        r.temperatureRaw = 2100;

        Hl2TelemetryService svc;
        Hl2TelemetryServiceTestAccess::placeReply(svc, r);
        const auto rows = svc.healthRows();

        check(rowValue(rows, "temperatureRaw").toInt() == 2100,
              "the raw count is still reported for anyone comparing paths");
        const QVariant degrees = rowValue(rows, "temperatureC");
        check(degrees.isValid(),
              "and the stream-free path fills PA temperature (°C) too");
        check(qFuzzyCompare(degrees.toDouble() + 1.0,
                            hl2TemperatureCelsius(2100) + 1.0),
              "with the SAME conversion the in-band path uses, not a re-typed one");

        // No reply at all must stay ABSENT rather than becoming 0 °C, which is
        // a real temperature and would read as a measurement.
        Hl2TelemetryService cold;
        check(!rowValue(cold.healthRows(), "temperatureC").isValid(),
              "with nothing received, the degrees row is absent — not 0 °C");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_service_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
