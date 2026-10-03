// CTR2 USB link codec (wire format v0): known-answer vectors, round trips
// and fail-closed reassembly. Socket-free and device-free.

#include "core/Ctr2HidFraming.h"
#include "ctr2_hid_vectors.h"

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

std::vector<Report> toReports(const ctr2vectors::Vector& v)
{
    std::vector<Report> out;
    for (int i = 0; i < v.reportCount; ++i) {
        Report r{};
        std::copy(v.reports[i], v.reports[i] + kReportBytes, r.begin());
        out.push_back(r);
    }
    return out;
}

// Encodes each vector from a fresh encoder at the vector's counter and
// compares report bytes exactly; then decodes them back.
void testKnownAnswerVectors()
{
    for (const ctr2vectors::Vector& v : ctr2vectors::kVectors) {
        FrameEncoder enc;
        std::vector<Report> scratch;
        while (enc.counter() != v.counter) {
            enc.encodeControl(MessageType::Hello, &scratch);
        }
        std::vector<Report> got;
        const QByteArray payload(reinterpret_cast<const char*>(v.payload), v.payloadLength);
        if (v.type == 0) {
            enc.encodeData(payload, &got);
        } else if (v.type == 4) {
            enc.encodeDatagram(v.port, payload, &got);
        } else {
            enc.encodeControl(static_cast<MessageType>(v.type), &got);
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "vector '%s' encodes byte-exact", v.name);
        check(got == toReports(v), msg);

        FrameReassembler dec;
        std::vector<Message> msgs;
        if (v.type == 0 || v.type == 4) {
            const Report start{kMarker, kVersion,
                               static_cast<std::uint8_t>((v.counter - 1) & kCounterMask),
                               static_cast<std::uint8_t>(MessageType::Ready), 0, 1, 0, 0};
            dec.feed(start, &msgs);
            msgs.clear();
        }
        bool ok = true;
        for (const Report& r : got) {
            ok = dec.feed(r, &msgs) && ok;
        }
        std::snprintf(msg, sizeof msg, "vector '%s' decodes to its message", v.name);
        check(ok && msgs.size() == 1 && static_cast<int>(msgs[0].type) == v.type
                  && msgs[0].payload == payload && msgs[0].port == v.port,
              msg);
    }
}

QByteArray streamRoundTrip(const QByteArray& payload, bool* ok)
{
    FrameEncoder enc;
    FrameReassembler dec;
    std::vector<Report> reports;
    enc.encodeControl(MessageType::Hello, &reports);
    enc.encodeData(payload, &reports);
    std::vector<Message> msgs;
    bool good = true;
    for (const Report& r : reports) {
        good = dec.feed(r, &msgs) && good;
    }
    QByteArray out;
    for (const Message& m : msgs) {
        if (m.type == MessageType::Data) {
            out += m.payload;
        }
    }
    *ok = good && !dec.isMidMessage() && !msgs.empty() && msgs[0].type == MessageType::Hello;
    return out;
}

void testRoundTrips()
{
    QByteArray all;
    for (int v = 0; v < 256; ++v) {
        all.append(static_cast<char>(v));
    }
    bool ok = false;
    check(streamRoundTrip(all, &ok) == all && ok, "every byte value, including 0x00 and 0xFF, survives");
    for (int len : {1, 6, 7, 8, 13, 14, 15, kMaxPayloadBytes - 1, kMaxPayloadBytes,
                    kMaxPayloadBytes + 1, 20000}) {
        const QByteArray p = randomBytes(len, static_cast<unsigned>(len));
        check(streamRoundTrip(p, &ok) == p && ok, "length round trip is exact; padding never released");
    }
    // Trailing zeros in the payload are payload, not padding.
    const QByteArray zeros("cmd\n\0\0\0", 7);
    check(streamRoundTrip(zeros, &ok) == zeros && ok, "payload zeros survive (exact length, not zero-stripping)");
}

void testCounterWrapAndChunking()
{
    FrameEncoder enc;
    FrameReassembler dec;
    std::vector<Report> reports;
    enc.encodeControl(MessageType::Ready, &reports);
    const QByteArray p = randomBytes(300 * 5, 3);
    for (int i = 0; i < 300; ++i) {  // 300 messages: counter wraps twice
        enc.encodeData(p.mid(i * 5, 5), &reports);
    }
    check(enc.counter() == (301 & kCounterMask), "encoder counter wraps 0x7F -> 0x00");
    bool sevenBit = true;
    for (const Report& r : reports) {
        sevenBit = sevenBit && (r[0] == kMarker || r[0] <= kCounterMask);
    }
    check(sevenBit, "byte 0 is 0xFF for headers and never above 0x7F for data");
    std::vector<Message> msgs;
    bool good = true;
    for (const Report& r : reports) {
        good = dec.feed(r, &msgs) && good;
        check(dec.bufferedBytes() <= kMaxPayloadBytes, "reassembly buffer within the maximum");
    }
    QByteArray out;
    for (const Message& m : msgs) {
        out += m.payload;
    }
    check(good && out == p, "300 messages across counter wrap reassemble in order");
}

void testControlMessagesResync()
{
    FrameEncoder dev;
    FrameReassembler host;
    std::vector<Report> r;
    dev.encodeControl(MessageType::Hello, &r);
    dev.encodeData(QByteArray("a\n"), &r);
    dev.encodeData(QByteArray("b\n"), &r);
    // Device restarts mid-stream: HELLO with counter 0 again.
    dev.reset();
    dev.encodeControl(MessageType::Hello, &r);
    dev.encodeData(QByteArray("c\n"), &r);
    std::vector<Message> msgs;
    bool good = true;
    for (const Report& rep : r) {
        good = host.feed(rep, &msgs) && good;
    }
    check(good && msgs.size() == 5 && msgs[3].type == MessageType::Hello
              && msgs[4].payload == QByteArray("c\n"),
          "a new HELLO resynchronizes the counter");
}

void testDatagrams()
{
    FrameEncoder enc;
    FrameReassembler dec;
    std::vector<Report> r;
    enc.encodeControl(MessageType::Ready, &r);
    const QByteArray big = randomBytes(kMaxDatagramBytes, 77);
    const QByteArray small("\x00\xff", 2);
    check(enc.encodeDatagram(4992, big, &r), "a 1472-byte datagram is accepted");
    enc.encodeData(QByteArray("between\n"), &r);
    check(enc.encodeDatagram(4991, small, &r), "a 2-byte datagram is accepted");
    std::vector<Report> none;
    check(!enc.encodeDatagram(4992, randomBytes(kMaxDatagramBytes + 1, 1), &none)
              && !enc.encodeDatagram(4992, QByteArray(), &none) && none.empty(),
          "oversize and empty datagrams are refused without emitting reports");
    std::vector<Message> msgs;
    bool ok = true;
    for (const Report& rep : r) {
        ok = dec.feed(rep, &msgs) && ok;
    }
    check(ok && msgs.size() == 4, "datagrams share the counter sequence with DATA");
    check(msgs.size() == 4 && msgs[1].type == MessageType::Datagram && msgs[1].port == 4992
              && msgs[1].payload == big,
          "datagram boundary, port and bytes survive");
    check(msgs.size() == 4 && msgs[2].type == MessageType::Data && msgs[2].payload == "between\n",
          "DATA and DATAGRAM interleave by whole message");
    check(msgs.size() == 4 && msgs[3].port == 4991 && msgs[3].payload == small,
          "a datagram with 0x00/0xFF bytes is exact");
}

void expectFailure(const char* what, const std::vector<Report>& reports,
                   FrameReassembler::Error expected, bool startFirst = true)
{
    FrameReassembler dec;
    std::vector<Message> msgs;
    if (startFirst) {
        dec.feed(Report{kMarker, kVersion, 0x7F, 0x01, 0, 1, 0, 0}, &msgs);  // HELLO, next = 0
        msgs.clear();
    }
    bool rejected = false;
    for (const Report& r : reports) {
        if (!dec.feed(r, &msgs)) {
            rejected = true;
            break;
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s: rejected with the right error", what);
    check(rejected && dec.error() == expected, msg);
    std::snprintf(msg, sizeof msg, "%s: no partial payload released", what);
    check(msgs.empty(), msg);
    std::vector<Report> good;
    FrameEncoder enc;
    enc.encodeControl(MessageType::Hello, &good);
    enc.encodeData(QByteArray("ok"), &good);
    for (const Report& r : good) {
        dec.feed(r, &msgs);
    }
    std::snprintf(msg, sizeof msg, "%s: failure is sticky until reset", what);
    check(msgs.empty() && dec.failed(), msg);
    dec.reset();
    for (const Report& r : good) {
        dec.feed(r, &msgs);
    }
    std::snprintf(msg, sizeof msg, "%s: reset starts fresh", what);
    check(msgs.size() == 2 && msgs[1].payload == QByteArray("ok"), msg);
}

void testFailClosed()
{
    using E = FrameReassembler::Error;
    expectFailure("data report where a header belongs", {Report{0, 1, 2, 3, 4, 5, 6, 7}}, E::BadMarker);
    expectFailure("wrong version", {Report{kMarker, 1, 0, 0, 0, 2, 0, 1}}, E::BadVersion);
    expectFailure("counter above 0x7F", {Report{kMarker, kVersion, 0x80, 0, 0, 2, 0, 1}}, E::BadCounter);
    expectFailure("unknown type", {Report{kMarker, kVersion, 0, 5, 0, 1, 0, 0}}, E::BadType);
    expectFailure("DATAGRAM without a datagram byte", {Report{kMarker, kVersion, 0, 4, 0, 2, 0, 2}},
                  E::BadLength);
    expectFailure("DATAGRAM over 1472 bytes",
                  {Report{kMarker, kVersion, 0, 4, 0x00, 0xD4, 0x05, 0xC3}}, E::BadLength);
    expectFailure("DATAGRAM before any control message",
                  {Report{kMarker, kVersion, 0, 4, 0, 2, 0, 3}}, E::NotStarted, false);
    expectFailure("empty DATA", {Report{kMarker, kVersion, 0, 0, 0, 1, 0, 0}}, E::BadLength);
    expectFailure("control with payload", {Report{kMarker, kVersion, 0, 1, 0, 2, 0, 1}}, E::BadLength);
    expectFailure("DATA over 512 bytes", {Report{kMarker, kVersion, 0, 0, 0, 75, 0x02, 0x01}}, E::BadLength);
    expectFailure("packet count disagrees", {Report{kMarker, kVersion, 0, 0, 0, 3, 0, 7}},
                  E::PacketCountMismatch);
    expectFailure("DATA before any control message", {Report{kMarker, kVersion, 0, 0, 0, 2, 0, 1}},
                  E::NotStarted, false);
    expectFailure("skipped message", {Report{kMarker, kVersion, 1, 0, 0, 2, 0, 1}}, E::CounterMismatch);
    expectFailure("data report counter disagrees",
                  {Report{kMarker, kVersion, 0, 0, 0, 3, 0, 10}, Report{0, 1, 2, 3, 4, 5, 6, 7},
                   Report{1, 8, 9, 10, 0, 0, 0, 0}},
                  E::DataCounterMismatch);
    expectFailure("truncated message then a new header",
                  {Report{kMarker, kVersion, 0, 0, 0, 3, 0, 10}, Report{0, 1, 2, 3, 4, 5, 6, 7},
                   Report{kMarker, kVersion, 1, 0, 0, 2, 0, 1}},
                  E::DataCounterMismatch);
    FrameReassembler dec;
    std::vector<Message> msgs;
    const std::uint8_t withReportId[9] = {kReportId, kMarker, kVersion, 0, 1, 0, 1, 0, 0};
    check(!dec.feed(withReportId, 9, &msgs) && dec.error() == E::BadReportSize,
          "a buffer that still carries the report ID is rejected, not misparsed");
}

void testMidMessageHeld()
{
    FrameReassembler dec;
    std::vector<Message> msgs;
    dec.feed(Report{kMarker, kVersion, 5, 2, 0, 1, 0, 0}, &msgs);  // READY, next = 6
    msgs.clear();
    dec.feed(Report{kMarker, kVersion, 6, 0, 0, 3, 0, 9}, &msgs);
    dec.feed(Report{6, 1, 2, 3, 4, 5, 6, 7}, &msgs);
    check(dec.isMidMessage() && msgs.empty() && dec.bufferedBytes() == 7,
          "an incomplete message is held, not released");
    dec.reset();
    check(!dec.isMidMessage() && dec.bufferedBytes() == 0, "reset discards it");
}

} // namespace

int main()
{
    testKnownAnswerVectors();
    testRoundTrips();
    testCounterWrapAndChunking();
    testControlMessagesResync();
    testDatagrams();
    testFailClosed();
    testMidMessageHeld();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_framing_test: all checks passed\n");
    return 0;
}
