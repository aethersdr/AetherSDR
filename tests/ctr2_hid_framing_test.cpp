// CTR2 USB link codec (wire format v0): known-answer vectors, round trips
// and fail-closed reassembly. Socket-free and device-free.

#include "core/Ctr2HidFraming.h"
#include "ctr2_hid_vectors.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <limits>
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

// Extensions: rejected unless negotiated, sequenced like Data when they are.
void testExtensions()
{
    const QByteArray payload = spectrum::encode(67, 4000, std::vector<float>(32, -45.0f));
    FrameEncoder enc;
    std::vector<Report> reports;
    enc.encodeControl(MessageType::Ready, &reports);
    check(enc.encodeExtension(MessageType::AudioSpectrum, payload, &reports),
          "an extension message encodes");
    check(!enc.encodeExtension(MessageType::Data, payload, &reports),
          "encodeExtension refuses a version-0 type");
    check(!enc.encodeExtension(MessageType::AudioSpectrum, {}, &reports),
          "encodeExtension refuses an empty payload");
    check(reports.size() == 1 + static_cast<size_t>(packetsFor(payload.size())),
          "READY plus one extension message were emitted");
    check(reports[1][3] == 0x40, "AudioSpectrum is type 0x40");

    FrameReassembler plain;
    std::vector<Message> out;
    bool ok = true;
    for (const Report& r : reports) {
        ok = plain.feed(r, &out) && ok;
    }
    check(!ok && plain.error() == FrameReassembler::Error::BadType,
          "an extension the receiver did not negotiate is a framing error (stock CTR2 view)");

    FrameReassembler negotiated;
    negotiated.setExtensions(capabilityBit(MessageType::AudioSpectrum));
    negotiated.reset();  // the negotiated set survives a link restart
    out.clear();
    ok = true;
    for (const Report& r : reports) {
        ok = negotiated.feed(r, &out) && ok;
    }
    check(ok && out.size() == 2 && out[1].type == MessageType::AudioSpectrum
              && out[1].payload == payload,
          "a negotiated extension arrives whole with its type");

    // Data after an extension keeps the shared counter sequence.
    std::vector<Report> more;
    enc.encodeData(QByteArray("ping\n"), &more);
    for (const Report& r : more) {
        ok = negotiated.feed(r, &out) && ok;
    }
    check(ok && out.back().type == MessageType::Data, "Data follows an extension in sequence");
}

// Each extension type has its own maximum length, and only defined types
// can be enabled.
void testExtensionLengths()
{
    check(extensionMaxLength(MessageType::AudioSpectrum) == 5 + spectrum::kMaxBars,
          "AudioSpectrum carries at most 69 bytes");
    FrameEncoder enc;
    std::vector<Report> out;
    check(!enc.encodeExtension(MessageType::AudioSpectrum, QByteArray(70, '\x01'), &out)
              && out.empty(),
          "the encoder refuses an over-long spectrum");

    FrameReassembler rx;
    rx.setExtensions(0xFFFFFFFFu);  // every bit offered
    std::vector<Message> msgs;
    enc.encodeControl(MessageType::Ready, &out);
    for (const Report& r : out) {
        rx.feed(r, &msgs);
    }
    // A hand-built header: AudioSpectrum claiming 1474 bytes.
    const int len = 1474;
    const int packets = packetsFor(len);
    const Report big{kMarker, kVersion, 1, 0x40, std::uint8_t(packets >> 8),
                     std::uint8_t(packets & 0xFF), std::uint8_t(len >> 8), std::uint8_t(len & 0xFF)};
    check(!rx.feed(big, &msgs) && rx.error() == FrameReassembler::Error::BadLength,
          "an over-long extension header is a framing error");

    FrameReassembler undefined;
    undefined.setExtensions(1u << 5);  // type 0x45: not defined
    out.clear();
    msgs.clear();
    enc.reset();
    enc.encodeControl(MessageType::Ready, &out);
    for (const Report& r : out) {
        undefined.feed(r, &msgs);
    }
    const Report undef{kMarker, kVersion, 1, 0x45, 0, 2, 0, 1};
    check(!undefined.feed(undef, &msgs) && undefined.error() == FrameReassembler::Error::BadType,
          "enabling a type with no defined length does not make it acceptable");
}

void testCapabilitiesAndSpectrum()
{
    const std::uint32_t caps = capabilityBit(MessageType::AudioSpectrum) | (1u << 31);
    const QByteArray r = capabilities::encode(caps);
    check(r.size() == 8 && r[0] == 'C' && r[1] == 'X' && r[2] == 0x01,
          "capabilities report starts 'C' 'X' version 1");
    check(capabilities::decode(r) == caps, "capabilities round-trip");
    QByteArray bad = r;
    bad[0] = 'Z';
    check(capabilities::decode(bad) == 0, "a report without the magic offers nothing");
    check(capabilities::decode(r.left(7)) == 0, "a short report offers nothing");
    bad = r;
    bad[2] = 0x02;
    check(capabilities::decode(bad) == 0, "an unknown extension version offers nothing");

    const QByteArray p = spectrum::encode(67, 4000, {-90.0f, 0.0f, -45.0f, 5.0f, -200.0f,
                                                     std::numeric_limits<float>::quiet_NaN()});
    auto u = [&](int i) { return static_cast<std::uint8_t>(p[i]); };
    check(p.size() == 5 + 6 && u(0) == 6, "bar count leads the payload");
    check(u(1) == 0x00 && u(2) == 67, "then the log axis's low edge, 67 Hz, MSB first");
    check(u(3) == 0x0F && u(4) == 0xA0, "then the span, 4000 Hz, MSB first");
    check(u(5) == 0 && u(6) == 255 && u(7) == 128,
          "-90 dB is 0, 0 dBFS is 255, -45 dB is mid-scale");
    check(u(8) == 255 && u(9) == 0 && u(10) == 0,
          "levels clamp, and a non-finite level reads as the floor");
    check(spectrum::encode(67, 4000, std::vector<float>(100, -10.0f)).size()
              == 5 + spectrum::kMaxBars,
          "at most 64 bars are sent");
}

// The bars span the passband width, rounded up to a clean step, capped at
// the audio's Nyquist frequency; there is no per-mode rule.
void testDisplaySpan()
{
    check(spectrum::displaySpanHz(100, 2800, 24000) == 3000, "SSB 100-2800 spans 0-3 kHz");
    check(spectrum::displaySpanHz(0, 6000, 24000) == 6000, "a 6 kHz SSB filter spans 0-6 kHz");
    check(spectrum::displaySpanHz(-2800, -100, 24000) == 3000, "LSB is the same width");
    check(spectrum::displaySpanHz(-300, 300, 24000) == 600, "a 600 Hz CW filter spans 0-600 Hz");
    check(spectrum::displaySpanHz(-50, 50, 24000) == 500, "very narrow filters span at least 500 Hz");
    check(spectrum::displaySpanHz(-5000, 5000, 24000) == 10000, "AM +/-5 kHz spans 0-10 kHz");
    check(spectrum::displaySpanHz(-10000, 10000, 24000) == 12000,
          "AM +/-10 kHz is capped at 12 kHz, the Nyquist frequency of 24 kHz audio");
    check(spectrum::displaySpanHz(-10000, 10000, 48000) == 20000, "48 kHz audio allows 20 kHz");
    check(spectrum::displaySpanHz(0, 0, 24000) == spectrum::kDefaultSpanHz, "no passband: default");

    // The frequency axis is logarithmic from a low edge of span / 60, kept
    // within 20..100 Hz.
    check(spectrum::displayLowHz(6000) == 100 && spectrum::displayLowHz(3000) == 50
              && spectrum::displayLowHz(600) == 20 && spectrum::displayLowHz(12000) == 100,
          "low edges: 6 kHz -> 100 Hz, 3 kHz -> 50 Hz, 600 Hz -> 20 Hz, capped at 100 Hz");
    check(spectrum::bandEdgeHz(100, 6000, 32, 0) == 100 && spectrum::bandEdgeHz(100, 6000, 32, 32) == 6000,
          "the bars run from the low edge to the span");
    const double r0 = spectrum::bandEdgeHz(100, 6000, 32, 1) / spectrum::bandEdgeHz(100, 6000, 32, 0);
    const double r1 = spectrum::bandEdgeHz(100, 6000, 32, 20) / spectrum::bandEdgeHz(100, 6000, 32, 19);
    check(std::abs(r0 - r1) < 1e-9 && r0 > 1.13 && r0 < 1.14,
          "every bar covers the same frequency ratio (log spacing)");
    check(std::abs(spectrum::bandEdgeHz(100, 6000, 32, 16) - 774.6) < 0.1,
          "the middle bar edge is the geometric mean, 775 Hz, not 3 kHz");
    check(spectrum::bandEdgeHz(0, 4000, 32, 8) == 1000, "a low edge of 0 means linear spacing");
}

int main()
{
    testKnownAnswerVectors();
    testRoundTrips();
    testCounterWrapAndChunking();
    testControlMessagesResync();
    testDatagrams();
    testFailClosed();
    testMidMessageHeld();
    testExtensions();
    testCapabilitiesAndSpectrum();
    testDisplaySpan();
    testExtensionLengths();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_framing_test: all checks passed\n");
    return 0;
}
