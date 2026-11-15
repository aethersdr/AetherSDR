// WsjtxDialTracker unit tests (#3595).
//
// SpotHub's WSJT-X UDP feed used to keep ONE dial frequency — whichever
// Status datagram arrived last — and add every Decode's audio offset to it.
// WSJT-X Decode messages carry only the offset (0–5000 Hz), so with two
// instances sharing the port (20 m FT8 on slice A, 40 m FT8 on slice B) a
// 40 m decode that landed between two 20 m Status messages was placed at
// 14.07x MHz and painted on the 20 m panadapter. The fix keys the dial on
// the instance id every WSJT-X datagram begins with.
//
// The tracker is header-only and Qt-Core-only so these checks run without
// WsjtxClient's QUdpSocket / LogManager dependency graph. The datagram
// framing itself (magic, schema, Qt-serialised strings) is unchanged by the
// fix and is not what this test pins.

#include "core/WsjtxDialTracker.h"

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>
#include <optional>

namespace AetherSDR {

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static bool approxEq(std::optional<double> v, double expected)
{
    return v.has_value() && std::abs(*v - expected) < 0.5;
}

const QString kInstanceA = QStringLiteral("WSJT-X");
const QString kInstanceB = QStringLiteral("WSJT-X - 2");
constexpr double k20mFt8Hz = 14074000.0;
constexpr double k40mFt8Hz = 7074000.0;

// The exact interleaving from the report: A reports 20 m, B reports 40 m,
// then A reports again. A decode from B must still resolve to 40 m.
static void testDecodeUsesItsOwnInstancesDial()
{
    WsjtxDialTracker t;
    t.noteStatus(kInstanceA, k20mFt8Hz);
    t.noteStatus(kInstanceB, k40mFt8Hz);
    t.noteStatus(kInstanceA, k20mFt8Hz);   // the "last Status wins" trap

    check(approxEq(t.dialFreqHzFor(kInstanceB), k40mFt8Hz),
          "B's decode resolves against B's 40 m dial even after A's later Status");
    check(approxEq(t.dialFreqHzFor(kInstanceA), k20mFt8Hz),
          "A's decode resolves against A's 20 m dial");
    check(t.instanceCount() == 2, "two instances are tracked independently");
}

// Without a Status from that instance there is no band to place the decode
// on; the tracker must refuse rather than hand back another instance's dial.
static void testUnknownInstanceIsRefused()
{
    WsjtxDialTracker t;
    check(!t.dialFreqHzFor(kInstanceA).has_value(),
          "an empty tracker places nothing");

    t.noteStatus(kInstanceA, k20mFt8Hz);
    check(!t.dialFreqHzFor(kInstanceB).has_value(),
          "an instance that has never reported a dial is refused, not given A's — "
          "this is the exact leak: B's decode on A's band");
}

// A band change on one instance must not move the other.
static void testBandChangeIsPerInstance()
{
    WsjtxDialTracker t;
    t.noteStatus(kInstanceA, k20mFt8Hz);
    t.noteStatus(kInstanceB, k40mFt8Hz);

    t.noteStatus(kInstanceA, 21074000.0);   // A QSYs to 15 m
    check(approxEq(t.dialFreqHzFor(kInstanceA), 21074000.0),
          "A follows its own QSY to 15 m");
    check(approxEq(t.dialFreqHzFor(kInstanceB), k40mFt8Hz),
          "B stays on 40 m when A changes band");
}

// WSJT-X reports dial 0 while it has no rig connection. Adding a 1.5 kHz
// audio offset to 0 Hz would 'place' the decode at 1.5 kHz; that Status must
// be ignored, and it must not wipe a good dial already known for the id.
static void testZeroDialIsIgnored()
{
    WsjtxDialTracker t;
    t.noteStatus(kInstanceA, 0.0);
    check(!t.dialFreqHzFor(kInstanceA).has_value(),
          "a 0 Hz dial (no rig) does not register an instance");

    t.noteStatus(kInstanceA, k20mFt8Hz);
    t.noteStatus(kInstanceA, 0.0);          // rig link drops momentarily
    check(approxEq(t.dialFreqHzFor(kInstanceA), k20mFt8Hz),
          "a transient 0 Hz Status does not overwrite the last good dial");
}

// A Close datagram means that instance is gone; a relaunch must start from
// its own first Status rather than inherit the old band.
static void testCloseForgetsTheInstance()
{
    WsjtxDialTracker t;
    t.noteStatus(kInstanceA, k20mFt8Hz);
    t.noteStatus(kInstanceB, k40mFt8Hz);

    t.forget(kInstanceB);
    check(!t.dialFreqHzFor(kInstanceB).has_value(),
          "a closed instance is forgotten");
    check(approxEq(t.dialFreqHzFor(kInstanceA), k20mFt8Hz),
          "forgetting B leaves A intact");
    check(t.instanceCount() == 1, "instance count drops on Close");

    t.forget(QStringLiteral("never-seen"));
    check(t.instanceCount() == 1, "forgetting an unknown id is a no-op");

    t.clear();
    check(t.instanceCount() == 0, "clear() empties the tracker");
}

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AetherSDR::testDecodeUsesItsOwnInstancesDial();
    AetherSDR::testUnknownInstanceIsRefused();
    AetherSDR::testBandChangeIsPerInstance();
    AetherSDR::testZeroDialIsIgnored();
    AetherSDR::testCloseForgetsTheInstance();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "wsjtx_dial_tracker_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
