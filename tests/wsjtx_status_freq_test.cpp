// resolveWsjtxFreq() unit tests (#4526).
//
// WsjtxClient::parseStatus() extends the #3595 dial-frequency parsing to
// also read WSJT-X's Rx DF / Tx DF audio offsets, so the panadapter overlay
// can show WSJT-X's live Rx/Tx frequency. This pins the pure math: how the
// dial and offsets combine, and — the actually risky part — what happens
// when the offset fields could not be read at all (older WSJT-X schema, or
// a datagram truncated in flight).
//
// Header-only and Qt-Core-only, same lane as wsjtx_dial_tracker_test: it
// runs without WsjtxClient's QUdpSocket/LogManager dependency graph. The
// QDataStream byte-parsing itself (field order, string/bool framing) is not
// pinned here — see the PR notes for that known gap.

#include "core/WsjtxStatusFreq.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

namespace AetherSDR {

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static bool approxEq(double a, double b)
{
    return std::abs(a - b) < 0.5;
}

constexpr double k20mDialHz = 14074000.0;

// The common case: WSJT-X reports a 1500 Hz Rx offset and a matching Tx
// offset (mox off, so Tx tracks Rx) — both should land at dial + offset.
static void testOffsetsAddToDial()
{
    const auto r = resolveWsjtxFreq(k20mDialHz, /*hasOffsets=*/true,
                                     /*rxDfHz=*/1500, /*txDfHz=*/1500);
    check(approxEq(r.rxFreqHz, k20mDialHz + 1500.0), "Rx = dial + Rx DF");
    check(approxEq(r.txFreqHz, k20mDialHz + 1500.0), "Tx = dial + Tx DF");
}

// Split operation: Tx DF genuinely differs from Rx DF (e.g. answering a
// station decoded elsewhere in the passband).
static void testSplitRxTxOffsets()
{
    const auto r = resolveWsjtxFreq(k20mDialHz, /*hasOffsets=*/true,
                                     /*rxDfHz=*/800, /*txDfHz=*/2200);
    check(approxEq(r.rxFreqHz, k20mDialHz + 800.0), "split: Rx uses Rx DF");
    check(approxEq(r.txFreqHz, k20mDialHz + 2200.0), "split: Tx uses Tx DF");
    check(std::abs(r.txFreqHz - r.rxFreqHz) > 1.0,
          "split: Rx and Tx are genuinely different frequencies");
}

// A zero offset is legitimate (signal right at the edge of the passband) and
// must be honored, not treated as "no offset known".
static void testZeroOffsetIsHonored()
{
    const auto r = resolveWsjtxFreq(k20mDialHz, /*hasOffsets=*/true,
                                     /*rxDfHz=*/0, /*txDfHz=*/0);
    check(approxEq(r.rxFreqHz, k20mDialHz), "a real 0 Hz offset still resolves to exactly the dial");
    check(approxEq(r.txFreqHz, k20mDialHz), "same for Tx DF = 0");
}

// The refuse-rather-than-guess case: an older/truncated Status did not carry
// usable offsets. Both frequencies must collapse to the dial — NOT to
// whatever rxDfHz/txDfHz happen to hold (parseStatus() passes 0 for those
// when hasOffsets is false, but this must not depend on that convention).
static void testNoOffsetsCollapsesToDial()
{
    const auto r = resolveWsjtxFreq(k20mDialHz, /*hasOffsets=*/false,
                                     /*rxDfHz=*/9999, /*txDfHz=*/9999);
    check(approxEq(r.rxFreqHz, k20mDialHz),
          "no offsets known: Rx falls back to the dial, ignoring whatever rxDfHz holds");
    check(approxEq(r.txFreqHz, k20mDialHz),
          "no offsets known: Tx falls back to the dial, ignoring whatever txDfHz holds");
}

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AetherSDR::testOffsetsAddToDial();
    AetherSDR::testSplitRxTxOffsets();
    AetherSDR::testZeroOffsetIsHonored();
    AetherSDR::testNoOffsetsCollapsesToDial();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "wsjtx_status_freq_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
