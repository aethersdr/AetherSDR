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
#include <cmath>
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

// The C reference and the application codec agree on extensions: constants,
// the capabilities report, and messages in both directions, gated alike.
void testExtensionsAgree()
{
    check(CTR2_EXT_FIRST == kExtensionFirst && CTR2_EXT_LAST == kExtensionLast
              && CTR2_EXT_AUDIO_SPECTRUM == static_cast<int>(MessageType::AudioSpectrum)
              && CTR2_CAP(CTR2_EXT_AUDIO_SPECTRUM) == capabilityBit(MessageType::AudioSpectrum)
              && CTR2_FEATURE_REPORT_ID == capabilities::kReportId
              && CTR2_FEATURE_BYTES == capabilities::kBytes
              && CTR2_SPECTRUM_MAX_BARS == spectrum::kMaxBars
              && CTR2_SPECTRUM_FLOOR_DB == spectrum::kFloorDb
              && CTR2_SPECTRUM_HEADER == spectrum::kHeaderBytes
              && ctr2_ext_max_length(CTR2_EXT_AUDIO_SPECTRUM)
                     == extensionMaxLength(MessageType::AudioSpectrum)
              && ctr2_ext_max_length(0x45) == 0,
          "extension constants and maximum lengths agree");
    // Both sides place the bars on the same log axis.
    bool edgesAgree = true;
    for (int span : {600, 3000, 6000, 12000}) {
        const int low = spectrum::displayLowHz(span);
        for (int i = 0; i <= 32; ++i) {
            const double cpp = spectrum::bandEdgeHz(low, span, 32, i);
            const double c = ctr2_spectrum_band_edge(std::uint16_t(low), std::uint16_t(span), 32,
                                                     std::uint8_t(i));
            edgesAgree = edgesAgree && std::abs(cpp - c) < 0.01 * cpp + 0.01;
        }
    }
    check(edgesAgree, "the reference and the application agree on every band edge");
    ctr2_rx masked{};
    ctr2_rx_set_extensions(&masked, 0xFFFFFFFFu);
    check(masked.extensions == CTR2_CAP(CTR2_EXT_AUDIO_SPECTRUM),
          "the reference keeps only defined extension types");
    const std::uint8_t big[CTR2_SPECTRUM_HEADER + CTR2_SPECTRUM_MAX_BARS + 1] = {};
    std::vector<Report> none;
    ctr2_tx t{};
    check(ctr2_tx_send_extension(&t, CTR2_EXT_AUDIO_SPECTRUM, big, sizeof big, collect, &none) == 0,
          "the reference refuses an over-long spectrum");

    const std::uint32_t caps = capabilityBit(MessageType::AudioSpectrum) | (1u << 7);
    std::uint8_t c[CTR2_FEATURE_BYTES];
    ctr2_caps_encode(caps, c);
    const QByteArray cpp = capabilities::encode(caps);
    check(std::equal(c, c + CTR2_FEATURE_BYTES, reinterpret_cast<const std::uint8_t*>(cpp.data())),
          "capabilities report bytes agree");
    std::uint32_t back = 0;
    check(ctr2_caps_decode(reinterpret_cast<const std::uint8_t*>(cpp.data()), cpp.size(), &back)
              && back == caps,
          "the C decoder reads the application's report");
    check(!ctr2_caps_decode(c, CTR2_FEATURE_BYTES - 1, &back), "the C decoder refuses a short report");

    const QByteArray payload = spectrum::encode(67, 4000, std::vector<float>(32, -30.0f));

    // Application -> C reference (host to device).
    FrameEncoder enc;
    std::vector<Report> reports;
    enc.encodeControl(MessageType::Ready, &reports);
    enc.encodeExtension(MessageType::AudioSpectrum, payload, &reports);
    ctr2_rx rx{};
    ctr2_rx_reset(&rx);
    ctr2_rx_set_extensions(&rx, CTR2_CAP(CTR2_EXT_AUDIO_SPECTRUM));
    ctr2_rx_reset(&rx);  // negotiated set survives a link restart
    std::uint8_t type = 0;
    const std::uint8_t* data = nullptr;
    std::uint16_t len = 0;
    int messages = 0;
    bool gotSpectrum = false;
    for (const Report& r : reports) {
        const ctr2_rx_result res = ctr2_rx_feed(&rx, r.data(), &type, &data, &len);
        check(res != CTR2_RX_ERROR, "the reference accepts a negotiated extension");
        if (res == CTR2_RX_MESSAGE) {
            ++messages;
            gotSpectrum = gotSpectrum
                || (type == CTR2_EXT_AUDIO_SPECTRUM && len == payload.size()
                    && std::equal(data, data + len,
                                  reinterpret_cast<const std::uint8_t*>(payload.data())));
        }
    }
    check(messages == 2 && gotSpectrum, "the reference reassembles the spectrum unchanged");

    ctr2_rx plain{};
    ctr2_rx_reset(&plain);
    bool rejected = false;
    for (const Report& r : reports) {
        rejected = rejected || ctr2_rx_feed(&plain, r.data(), &type, &data, &len) == CTR2_RX_ERROR;
    }
    check(rejected && plain.error == CTR2_ERR_BAD_TYPE,
          "the reference rejects an extension it did not negotiate");

    // C reference -> application (device to host).
    std::vector<Report> fromC;
    ctr2_tx tx{};
    ctr2_tx_reset(&tx);
    ctr2_tx_send(&tx, CTR2_TYPE_READY, nullptr, 0, collect, &fromC);
    check(ctr2_tx_send_extension(&tx, CTR2_EXT_AUDIO_SPECTRUM,
                                 reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 static_cast<std::uint16_t>(payload.size()), collect, &fromC) > 0,
          "the reference sends an extension");
    check(ctr2_tx_send_extension(&tx, CTR2_TYPE_DATA, reinterpret_cast<const std::uint8_t*>("x"), 1,
                                 collect, &fromC) == 0,
          "the reference refuses a version-0 type as an extension");
    FrameReassembler host;
    host.setExtensions(capabilityBit(MessageType::AudioSpectrum));
    std::vector<Message> out;
    bool ok = true;
    for (const Report& r : fromC) {
        ok = host.feed(r, &out) && ok;
    }
    check(ok && out.size() == 2 && out[1].type == MessageType::AudioSpectrum
              && out[1].payload == payload,
          "the application reassembles the reference's extension unchanged");
}

int main()
{
    testConstantsAgree();
    testExtensionsAgree();
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
