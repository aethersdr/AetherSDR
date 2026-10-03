// Holds the MIT firmware reference (tools/ctr2-firmware-reference, compiled
// as C) to the published vectors and to the application codec: each side
// must decode what the other encodes, and reject the same malformed input.

#include "core/Ctr2HidFraming.h"
#include "ctr2_hid_vectors.h"

extern "C" {
#include "ctr2_link.h"
}

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

void collect(void* ctx, const std::uint8_t report[CTR2_REPORT_BYTES])
{
    auto* out = static_cast<std::vector<Report>*>(ctx);
    Report r{};
    std::copy(report, report + CTR2_REPORT_BYTES, r.begin());
    out->push_back(r);
}

QByteArray randomBytes(int size, std::mt19937& rng)
{
    QByteArray out(size, '\0');
    for (int i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

void testConstantsAgree()
{
    check(CTR2_REPORT_BYTES == kReportBytes && CTR2_DATA_PER_REPORT == kDataBytesPerReport
              && CTR2_REPORT_ID == kReportId && CTR2_MARKER == kMarker
              && CTR2_VERSION == kVersion && CTR2_COUNTER_MASK == kCounterMask
              && CTR2_MAX_PAYLOAD == kMaxPayloadBytes
              && CTR2_TYPE_DATA == static_cast<int>(MessageType::Data)
              && CTR2_TYPE_HELLO == static_cast<int>(MessageType::Hello)
              && CTR2_TYPE_READY == static_cast<int>(MessageType::Ready)
              && CTR2_TYPE_CLOSED == static_cast<int>(MessageType::Closed)
              && CTR2_TYPE_DATAGRAM == static_cast<int>(MessageType::Datagram)
              && CTR2_MAX_DATAGRAM == kMaxDatagramBytes && CTR2_MAX_MESSAGE == kMaxMessageBytes,
          "reference constants match the application codec");
}

void testVectors()
{
    for (const ctr2vectors::Vector& v : ctr2vectors::kVectors) {
        ctr2_tx tx;
        ctr2_tx_reset(&tx);
        tx.counter = v.counter;
        std::vector<Report> got;
        const size_t n = v.type == CTR2_TYPE_DATAGRAM
            ? ctr2_tx_send_datagram(&tx, v.port, v.payload,
                                    static_cast<std::uint16_t>(v.payloadLength), collect, &got)
            : ctr2_tx_send(&tx, static_cast<std::uint8_t>(v.type), v.payload,
                           static_cast<std::uint16_t>(v.payloadLength), collect, &got);
        bool same = n == static_cast<size_t>(v.reportCount) && got.size() == n;
        for (size_t i = 0; same && i < n; ++i) {
            same = std::equal(got[i].begin(), got[i].end(), v.reports[i]);
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "reference encodes vector '%s' byte-exact", v.name);
        check(same, msg);

        ctr2_rx rx;
        ctr2_rx_reset(&rx);
        if (v.type == CTR2_TYPE_DATA || v.type == CTR2_TYPE_DATAGRAM) {
            const std::uint8_t ready[8] = {0xFF, 0x00,
                                           static_cast<std::uint8_t>((v.counter - 1) & 0x7F),
                                           CTR2_TYPE_READY, 0x00, 0x01, 0x00, 0x00};
            std::uint8_t t;
            const std::uint8_t* p;
            std::uint16_t l;
            ctr2_rx_feed(&rx, ready, &t, &p, &l);
        }
        int messages = 0;
        bool match = false;
        for (int i = 0; i < v.reportCount; ++i) {
            std::uint8_t t = 0xEE;
            const std::uint8_t* p = nullptr;
            std::uint16_t l = 0;
            if (ctr2_rx_feed(&rx, v.reports[i], &t, &p, &l) == CTR2_RX_MESSAGE) {
                ++messages;
                if (t == CTR2_TYPE_DATAGRAM) {
                    match = l == v.payloadLength + 2 && ((p[0] << 8) | p[1]) == v.port
                        && std::equal(p + 2, p + l, v.payload);
                } else {
                    match = t == v.type && l == v.payloadLength
                        && (l == 0 || std::equal(p, p + l, v.payload));
                }
            }
        }
        std::snprintf(msg, sizeof msg, "reference decodes vector '%s'", v.name);
        check(messages == 1 && match, msg);
    }
}

// Reference (firmware role) -> application (host role), then back.
void testCrossInterop()
{
    std::mt19937 rng(2026);
    // Device -> host: HELLO then commands of assorted sizes.
    ctr2_tx tx;
    ctr2_tx_reset(&tx);
    std::vector<Report> wire;
    ctr2_tx_send(&tx, CTR2_TYPE_HELLO, nullptr, 0, collect, &wire);
    QByteArray sent;
    std::vector<QByteArray> sentDatagrams;
    for (int i = 0; i < 400; ++i) {  // wraps the 7-bit counter
        if (i % 7 == 0) {
            const QByteArray dg = randomBytes(1 + static_cast<int>(rng() % CTR2_MAX_DATAGRAM), rng);
            ctr2_tx_send_datagram(&tx, 4992, reinterpret_cast<const std::uint8_t*>(dg.constData()),
                                  static_cast<std::uint16_t>(dg.size()), collect, &wire);
            sentDatagrams.push_back(dg);
        }
        const QByteArray cmd = randomBytes(1 + static_cast<int>(rng() % CTR2_MAX_PAYLOAD), rng);
        ctr2_tx_send(&tx, CTR2_TYPE_DATA, reinterpret_cast<const std::uint8_t*>(cmd.constData()),
                     static_cast<std::uint16_t>(cmd.size()), collect, &wire);
        sent += cmd;
    }
    FrameReassembler host;
    std::vector<Message> msgs;
    bool ok = true;
    for (const Report& r : wire) {
        ok = host.feed(r, &msgs) && ok;
    }
    QByteArray got;
    std::vector<QByteArray> gotDatagrams;
    bool ports = true;
    for (const Message& m : msgs) {
        if (m.type == MessageType::Data) {
            got += m.payload;
        } else if (m.type == MessageType::Datagram) {
            gotDatagrams.push_back(m.payload);
            ports = ports && m.port == 4992;
        }
    }
    check(ok && msgs.front().type == MessageType::Hello && got == sent,
          "application decodes the reference encoder's stream exactly");
    check(ports && gotDatagrams == sentDatagrams,
          "application decodes the reference encoder's datagrams exactly");

    // Host -> device: READY then an arbitrary radio stream.
    FrameEncoder enc;
    std::vector<Report> down;
    enc.encodeControl(MessageType::Ready, &down);
    const QByteArray radio = randomBytes(60000, rng);
    std::vector<QByteArray> downDatagrams;
    for (int pos = 0; pos < radio.size();) {
        const int n = std::min<int>(1 + static_cast<int>(rng() % 3000), radio.size() - pos);
        enc.encodeData(radio.mid(pos, n), &down);
        pos += n;
        const QByteArray dg = randomBytes(1 + static_cast<int>(rng() % CTR2_MAX_DATAGRAM), rng);
        enc.encodeDatagram(4991, dg, &down);
        downDatagrams.push_back(dg);
    }
    enc.encodeControl(MessageType::Closed, &down);
    ctr2_rx rx;
    ctr2_rx_reset(&rx);
    QByteArray rebuilt;
    std::vector<QByteArray> rebuiltDatagrams;
    bool dgPorts = true;
    int readies = 0;
    int closeds = 0;
    bool rxOk = true;
    for (const Report& r : down) {
        std::uint8_t t;
        const std::uint8_t* p;
        std::uint16_t l;
        const ctr2_rx_result res = ctr2_rx_feed(&rx, r.data(), &t, &p, &l);
        rxOk = rxOk && res != CTR2_RX_ERROR;
        if (res == CTR2_RX_MESSAGE) {
            readies += t == CTR2_TYPE_READY;
            closeds += t == CTR2_TYPE_CLOSED;
            if (t == CTR2_TYPE_DATA) {
                rebuilt.append(reinterpret_cast<const char*>(p), l);
            } else if (t == CTR2_TYPE_DATAGRAM) {
                dgPorts = dgPorts && ((p[0] << 8) | p[1]) == 4991;
                rebuiltDatagrams.emplace_back(reinterpret_cast<const char*>(p + 2), l - 2);
            }
        }
    }
    check(rxOk && readies == 1 && closeds == 1 && rebuilt == radio,
          "reference decodes the application encoder's stream exactly");
    check(dgPorts && rebuiltDatagrams == downDatagrams,
          "reference decodes the application encoder's datagrams exactly");
}

void testRejectSameInput()
{
    const std::vector<std::vector<Report>> bad = {
        {Report{0x00, 1, 2, 3, 4, 5, 6, 7}},
        {Report{0xFF, 0x01, 0, 0, 0, 2, 0, 1}},
        {Report{0xFF, 0x00, 0x80, 0, 0, 2, 0, 1}},
        {Report{0xFF, 0x00, 0, 5, 0, 1, 0, 0}},
        {Report{0xFF, 0x00, 0, 4, 0, 2, 0, 2}},
        {Report{0xFF, 0x00, 0, 4, 0x00, 0xD4, 0x05, 0xC3}},
        {Report{0xFF, 0x00, 0, 0, 0, 1, 0, 0}},
        {Report{0xFF, 0x00, 0, 1, 0, 2, 0, 1}},
        {Report{0xFF, 0x00, 0, 0, 0, 75, 0x02, 0x01}},
        {Report{0xFF, 0x00, 0, 0, 0, 3, 0, 7}},
        {Report{0xFF, 0x00, 1, 0, 0, 2, 0, 1}},
        {Report{0xFF, 0x00, 0, 0, 0, 3, 0, 10}, Report{0, 1, 2, 3, 4, 5, 6, 7},
         Report{1, 8, 9, 10, 0, 0, 0, 0}},
    };
    const Report hello{0xFF, 0x00, 0x7F, CTR2_TYPE_HELLO, 0, 1, 0, 0};
    int agree = 0;
    for (const auto& seq : bad) {
        FrameReassembler app;
        std::vector<Message> msgs;
        app.feed(hello, &msgs);
        bool appRejected = false;
        for (const Report& r : seq) {
            appRejected = appRejected || !app.feed(r, &msgs);
        }
        ctr2_rx rx;
        ctr2_rx_reset(&rx);
        std::uint8_t t;
        const std::uint8_t* p;
        std::uint16_t l;
        ctr2_rx_feed(&rx, hello.data(), &t, &p, &l);
        bool refRejected = false;
        for (const Report& r : seq) {
            refRejected = refRejected || ctr2_rx_feed(&rx, r.data(), &t, &p, &l) == CTR2_RX_ERROR;
        }
        agree += appRejected && refRejected;
    }
    check(agree == static_cast<int>(bad.size()), "reference and application reject the same malformed input");

    ctr2_tx tx;
    ctr2_tx_reset(&tx);
    std::vector<Report> none;
    const std::uint8_t byte = 'x';
    check(ctr2_tx_send(&tx, CTR2_TYPE_DATA, &byte, 0, collect, &none) == 0
              && ctr2_tx_send(&tx, CTR2_TYPE_HELLO, &byte, 1, collect, &none) == 0
              && ctr2_tx_send(&tx, CTR2_TYPE_DATA, &byte, CTR2_MAX_PAYLOAD + 1, collect, &none) == 0
              && ctr2_tx_send(&tx, CTR2_TYPE_DATAGRAM, &byte, 1, collect, &none) == 0
              && ctr2_tx_send_datagram(&tx, 4992, &byte, 0, collect, &none) == 0
              && ctr2_tx_send_datagram(&tx, 4992, &byte, CTR2_MAX_DATAGRAM + 1, collect, &none) == 0
              && none.empty() && tx.counter == 0,
          "reference sender refuses invalid messages without emitting reports");
}

} // namespace

int main()
{
    testConstantsAgree();
    testVectors();
    testCrossInterop();
    testRejectSameInput();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_reference_test: all checks passed\n");
    return 0;
}
