// Draft CTR2 HID framing codec: round trips and fail-closed reassembly.
// Socket-free and device-free; every case compares complete byte streams.

#include "core/Ctr2HidFraming.h"

#include <QByteArray>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace AetherSDR::ctr2hid;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

QByteArray randomBytes(int size, unsigned seed)
{
    std::mt19937 rng(seed);
    QByteArray out(size, '\0');
    for (int i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

QByteArray roundTrip(const QByteArray& payload, const WireFormat& format,
                     std::vector<Report>* reportsOut = nullptr, bool* ok = nullptr)
{
    FrameEncoder enc(format);
    FrameReassembler dec(format);
    std::vector<Report> reports;
    enc.encode(payload, &reports);
    QByteArray out;
    bool good = true;
    for (const Report& r : reports) {
        good = dec.feed(r, &out) && good;
    }
    if (reportsOut) {
        *reportsOut = reports;
    }
    if (ok) {
        *ok = good && !dec.isMidMessage();
    }
    return out;
}

Report header(const WireFormat& f, std::uint8_t counter, int packets, int length,
              std::uint8_t reserved = 0)
{
    return {f.marker, f.version, counter,
            static_cast<std::uint8_t>(packets & 0xFF), static_cast<std::uint8_t>(packets >> 8),
            static_cast<std::uint8_t>(length & 0xFF), static_cast<std::uint8_t>(length >> 8),
            reserved};
}

void testAllByteValues()
{
    QByteArray all;
    for (int v = 0; v < 256; ++v) {
        all.append(static_cast<char>(v));
    }
    all += QByteArray("\r\n\0\r", 4);
    bool ok = false;
    check(roundTrip(all, {}, nullptr, &ok) == all && ok, "every byte value survives a round trip");
}

void testLengthsAndPadding()
{
    WireFormat big;
    big.maxMessageBytes = 65535;
    for (int len : {1, 6, 7, 8, 13, 14, 15, 4096, 65535}) {
        const QByteArray p = randomBytes(len, static_cast<unsigned>(len));
        std::vector<Report> reports;
        bool ok = false;
        const QByteArray out = roundTrip(p, big, &reports, &ok);
        check(out == p && ok, "length round trip is exact (padding never released)");
        check(static_cast<int>(reports.size()) == 1 + packetsFor(len),
              "one header plus ceil(len/7) data reports");
    }
    // Final-report padding is not part of the payload, whatever its value.
    WireFormat f;
    FrameReassembler dec(f);
    QByteArray out;
    dec.feed(header(f, f.firstCounter, 1, 3), &out);
    dec.feed(Report{f.firstCounter, 'a', 'b', 'c', 0xFF, f.marker, 0x00, 0x7F}, &out);
    check(out == QByteArray("abc"), "exact length strips non-zero padding");
}

void testSplitAcrossMessagesAndCounterWrap()
{
    WireFormat f;
    f.maxMessageBytes = 10;
    const QByteArray p = randomBytes(10 * 300 + 3, 99);  // 301 messages: counter wraps
    FrameEncoder enc(f);
    std::vector<Report> reports;
    enc.encode(p, &reports);
    check(enc.nextCounter() == static_cast<std::uint8_t>(f.firstCounter + 301),
          "encoder counter wraps modulo 256");
    FrameReassembler dec(f);
    QByteArray out;
    bool good = true;
    for (const Report& r : reports) {
        good = dec.feed(r, &out) && good;
        check(dec.bufferedBytes() <= f.maxMessageBytes, "reassembly buffer stays within max");
        if (g_failures) {
            break;
        }
    }
    check(good && out == p, "a stream split into many messages reassembles in order across wrap");
}

void testIncrementalEncodeMatchesStream()
{
    // Chunk boundaries on the sending side need not match anything.
    FrameEncoder enc;
    FrameReassembler dec;
    const QByteArray p = randomBytes(20000, 5);
    std::mt19937 rng(6);
    QByteArray out;
    int pos = 0;
    while (pos < p.size()) {
        const int n = std::min<int>(static_cast<int>(rng() % 900) + 1, p.size() - pos);
        std::vector<Report> reports;
        enc.encode(p.mid(pos, n), &reports);
        for (const Report& r : reports) {
            dec.feed(r, &out);
        }
        pos += n;
    }
    check(!dec.failed() && out == p, "independent chunking reassembles exactly");
}

void testMarkerByteInsideDataIsData()
{
    WireFormat f;
    const QByteArray p(21, static_cast<char>(f.marker));
    QByteArray tricky;
    tricky.append(static_cast<char>(f.marker));
    tricky.append(static_cast<char>(f.version));
    tricky.append(p);
    bool ok = false;
    check(roundTrip(tricky, f, nullptr, &ok) == tricky && ok,
          "marker-valued payload bytes are never mistaken for a header");

    // A data report whose counter equals the marker value is still data.
    WireFormat w;
    w.firstCounter = w.marker;
    bool ok2 = false;
    check(roundTrip(randomBytes(30, 8), w, nullptr, &ok2) == randomBytes(30, 8) && ok2,
          "report position, not content, distinguishes header from data");
}

void expectFailure(const char* what, const std::vector<Report>& reports,
                   FrameReassembler::Error expected, const WireFormat& f = {})
{
    FrameReassembler dec(f);
    QByteArray out;
    bool anyFalse = false;
    for (const Report& r : reports) {
        if (!dec.feed(r, &out)) {
            anyFalse = true;
            break;
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s: rejected with the right error", what);
    check(anyFalse && dec.error() == expected, msg);
    std::snprintf(msg, sizeof msg, "%s: no partial payload released", what);
    check(out.isEmpty(), msg);
    // Failed state is sticky until reset.
    std::vector<Report> good;
    FrameEncoder enc(f);
    enc.encode(QByteArray("ok"), &good);
    for (const Report& r : good) {
        dec.feed(r, &out);
    }
    std::snprintf(msg, sizeof msg, "%s: input refused until reset", what);
    check(out.isEmpty() && dec.failed(), msg);
    dec.reset();
    for (const Report& r : good) {
        dec.feed(r, &out);
    }
    std::snprintf(msg, sizeof msg, "%s: reset starts fresh framing", what);
    check(out == QByteArray("ok") && !dec.failed(), msg);
}

void testFailClosed()
{
    const WireFormat f;
    const std::uint8_t c = f.firstCounter;
    expectFailure("data before header", {Report{c, 1, 2, 3, 4, 5, 6, 7}}, FrameReassembler::Error::BadMarker);
    {
        Report h = header(f, c, 1, 1);
        h[1] = static_cast<std::uint8_t>(f.version + 1);
        expectFailure("wrong version", {h}, FrameReassembler::Error::BadVersion);
    }
    expectFailure("reserved byte set", {header(f, c, 1, 1, 1)}, FrameReassembler::Error::BadReserved);
    expectFailure("first counter not fresh", {header(f, static_cast<std::uint8_t>(c + 5), 1, 1)},
                  FrameReassembler::Error::CounterMismatch);
    expectFailure("zero length", {header(f, c, 0, 0)}, FrameReassembler::Error::BadLength);
    expectFailure("length over max", {header(f, c, packetsFor(f.maxMessageBytes + 1), f.maxMessageBytes + 1)},
                  FrameReassembler::Error::BadLength);
    expectFailure("packet count disagrees", {header(f, c, 3, 8)},
                  FrameReassembler::Error::PacketCountMismatch);
    expectFailure("data counter disagrees",
                  {header(f, c, 2, 10), Report{c, 1, 2, 3, 4, 5, 6, 7},
                   Report{static_cast<std::uint8_t>(c + 1), 8, 9, 10, 0, 0, 0, 0}},
                  FrameReassembler::Error::DataCounterMismatch);
    // Truncated message followed by the next header: the header is consumed
    // as data and its counter byte (the marker) mismatches.
    expectFailure("truncated then new header",
                  {header(f, c, 2, 10), Report{c, 1, 2, 3, 4, 5, 6, 7},
                   header(f, static_cast<std::uint8_t>(c + 1), 1, 1)},
                  FrameReassembler::Error::DataCounterMismatch);
    {
        // Skipped message: second header jumps the counter.
        FrameEncoder enc(f);
        std::vector<Report> first;
        std::vector<Report> second;
        std::vector<Report> third;
        enc.encode(QByteArray("a"), &first);
        enc.encode(QByteArray("b"), &second);
        enc.encode(QByteArray("c"), &third);
        std::vector<Report> stream = first;
        stream.insert(stream.end(), third.begin(), third.end());
        FrameReassembler dec(f);
        QByteArray out;
        for (const Report& r : stream) {
            dec.feed(r, &out);
        }
        check(dec.error() == FrameReassembler::Error::CounterMismatch && out == QByteArray("a"),
              "a lost message is detected; only complete earlier messages were released");
    }
    {
        FrameReassembler dec(f);
        QByteArray out;
        const std::uint8_t shortReport[5] = {f.marker, f.version, c, 1, 0};
        check(!dec.feed(shortReport, 5, &out) && dec.error() == FrameReassembler::Error::BadReportSize,
              "a report that is not 8 bytes is rejected");
    }
}

void testMidMessageVisibility()
{
    const WireFormat f;
    FrameReassembler dec(f);
    QByteArray out;
    dec.feed(header(f, f.firstCounter, 2, 9), &out);
    check(dec.isMidMessage(), "mid-message after header");
    dec.feed(Report{f.firstCounter, 1, 2, 3, 4, 5, 6, 7}, &out);
    check(dec.isMidMessage() && out.isEmpty() && dec.bufferedBytes() == 7,
          "incomplete message is held, not released");
    dec.reset();
    check(!dec.isMidMessage() && dec.bufferedBytes() == 0, "reset discards the partial message");
}

} // namespace

int main()
{
    testAllByteValues();
    testLengthsAndPadding();
    testSplitAcrossMessagesAndCounterWrap();
    testIncrementalEncodeMatchesStream();
    testMarkerByteInsideDataIsData();
    testFailClosed();
    testMidMessageVisibility();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_framing_test: all checks passed\n");
    return 0;
}
