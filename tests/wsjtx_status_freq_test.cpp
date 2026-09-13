// resolveWsjtxFreq() / parseWsjtxStatusBody() unit tests (#4526).
//
// WsjtxClient::parseStatus() extends the #3595 dial-frequency parsing to
// also read WSJT-X's Rx DF / Tx DF audio offsets, so the panadapter overlay
// can show WSJT-X's live Rx/Tx frequency. This pins both halves: the pure
// math in resolveWsjtxFreq() (how the dial and offsets combine, and what
// happens when the offset fields could not be read at all), and, per RFC
// #4526 review item 1's explicit ask, the actual QDataStream byte-parsing
// in parseWsjtxStatusBody() — field order (DX Call/Report/Tx Mode/Tx
// Enabled/Transmitting/Decoding/[Rx DF/Tx DF]) is exactly the kind of thing
// that fails silently (resolveWsjtxFreq() quietly falls back to the dial)
// rather than crashing, so it needs to be pinned by a test rather than only
// verified by inspection.
//
// Header-only and Qt-Core-only, same lane as wsjtx_dial_tracker_test: it
// runs without WsjtxClient's QUdpSocket/LogManager dependency graph.
// parseWsjtxStatusBody() is fed hand-built QDataStream buffers using the
// same wsjtxReadQString/wsjtxReadBool primitives production uses (via
// WsjtxWireHelpers.h), so there is no second, potentially-divergent
// encoder/decoder pair here.

#include "core/WsjtxStatusFreq.h"
#include "core/WsjtxWireHelpers.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

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

// Mirrors wsjtxReadQString's wire format (Qt-style length-prefixed UTF-8,
// 0xFFFFFFFF length = null) so these tests build buffers the same way WSJT-X
// itself does, rather than via QDataStream's own (UTF-16-based) QString
// operator<<, which is a different wire format entirely.
static void writeWsjtxQString(QDataStream& ds, const QString& s)
{
    const QByteArray utf8 = s.toUtf8();
    ds << static_cast<quint32>(utf8.size());
    ds.writeRawData(utf8.constData(), utf8.size());
}

static void writeWsjtxBool(QDataStream& ds, bool b)
{
    ds << static_cast<quint8>(b ? 1 : 0);
}

// Builds a Status message body starting right after Id/Dial Frequency/Mode
// (which parseStatus() reads before handing the stream to
// parseWsjtxStatusBody()) — i.e. exactly what parseWsjtxStatusBody() itself
// consumes: DX Call, Report, Tx Mode, Tx Enabled, Transmitting, Decoding,
// and, when includeOffsets is true, Rx DF and Tx DF.
static QByteArray buildStatusBody(bool transmitting, bool includeOffsets,
                                   quint32 rxDfHz = 0, quint32 txDfHz = 0)
{
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);

    writeWsjtxQString(ds, "K1ABC");   // DX Call
    writeWsjtxQString(ds, "-10");     // Report
    writeWsjtxQString(ds, "FT8");     // Tx Mode
    writeWsjtxBool(ds, true);         // Tx Enabled
    writeWsjtxBool(ds, transmitting); // Transmitting
    writeWsjtxBool(ds, false);        // Decoding

    if (includeOffsets) {
        ds << rxDfHz << txDfHz;
    }

    return buf;
}

// The common case: a full, current-schema Status body with Rx/Tx DF
// present. Confirms field order — including the three strings ahead of the
// bools — is read correctly and the trailing offsets are picked up.
static void testParseBodyReadsFieldsInOrder()
{
    const QByteArray buf = buildStatusBody(/*transmitting=*/true, /*includeOffsets=*/true,
                                            /*rxDfHz=*/1500, /*txDfHz=*/2200);
    QDataStream ds(buf);
    ds.setByteOrder(QDataStream::BigEndian);

    const WsjtxStatusBody body = parseWsjtxStatusBody(ds);
    check(body.transmitting, "transmitting=true is read correctly, not skipped by a field misalignment");
    check(body.hasOffsets, "Rx/Tx DF present in the buffer is detected");
    check(body.rxDfHz == 1500, "Rx DF value survives the DX Call/Report/Tx Mode string walk intact");
    check(body.txDfHz == 2200, "Tx DF value survives the DX Call/Report/Tx Mode string walk intact");
}

// Transmitting=false must be read as false, not left at some default that
// happens to also be false — pins the exact bool position.
static void testParseBodyTransmittingFalse()
{
    const QByteArray buf = buildStatusBody(/*transmitting=*/false, /*includeOffsets=*/true, 800, 800);
    QDataStream ds(buf);
    ds.setByteOrder(QDataStream::BigEndian);

    const WsjtxStatusBody body = parseWsjtxStatusBody(ds);
    check(!body.transmitting, "transmitting=false is read correctly");
    check(body.hasOffsets, "offsets still present alongside transmitting=false");
}

// Older WSJT-X schemas (or a datagram truncated right after Decoding) don't
// carry Rx DF/Tx DF at all. parseWsjtxStatusBody() must notice the stream
// ended and report hasOffsets=false rather than reading garbage/zeros as if
// they were real offsets.
static void testParseBodyNoOffsetsInStream()
{
    const QByteArray buf = buildStatusBody(/*transmitting=*/false, /*includeOffsets=*/false);
    QDataStream ds(buf);
    ds.setByteOrder(QDataStream::BigEndian);

    const WsjtxStatusBody body = parseWsjtxStatusBody(ds);
    check(!body.hasOffsets, "stream ending after Decoding is recognized as \"no offsets\", not zeros");
    check(body.rxDfHz == 0 && body.txDfHz == 0, "no-offsets case leaves rxDfHz/txDfHz at their default");
}

// A zero Rx/Tx DF that IS present in the stream is a legitimate offset value
// and must be distinguished from "field absent" — same distinction
// resolveWsjtxFreq() relies on via hasOffsets.
static void testParseBodyZeroOffsetsPresent()
{
    const QByteArray buf = buildStatusBody(/*transmitting=*/false, /*includeOffsets=*/true, 0, 0);
    QDataStream ds(buf);
    ds.setByteOrder(QDataStream::BigEndian);

    const WsjtxStatusBody body = parseWsjtxStatusBody(ds);
    check(body.hasOffsets, "a real 0/0 Rx-Tx DF pair in the stream still counts as present");
    check(body.rxDfHz == 0 && body.txDfHz == 0, "the zero values themselves round-trip correctly");
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
    AetherSDR::testParseBodyReadsFieldsInOrder();
    AetherSDR::testParseBodyTransmittingFalse();
    AetherSDR::testParseBodyNoOffsetsInStream();
    AetherSDR::testParseBodyZeroOffsetsPresent();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "wsjtx_status_freq_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
