// aetherd HL2 Phase 1a — MetisProtocol unit test. Pins the HPSDR Protocol 1
// wire encoding/decoding ported from the live-validated prototypes/hl2 spike:
// C&C register encoding, discovery, EP2
// framing, and the 24-bit signed big-endian IQ decode (with sign-extension).
// Pure protocol — no sockets, no hardware.

#include "core/backends/hl2/MetisProtocol.h"

#include <array>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// Encode a 24-bit signed value big-endian into three bytes.
static void put24be(std::uint8_t* p, std::int32_t v)
{
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>(v & 0xFF);
}

// Build a synthetic EP6 packet whose sample n has I = 100*n, Q = -(100*n + 1).
static std::vector<std::uint8_t> makeEp6(std::uint32_t seq)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
    pkt[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    int n = 0;
    for (std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}}) {
        pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;        // SYNC
        std::uint8_t* payload = pkt.data() + fs + 8;       // after SYNC(3)+C&C(5)
        for (std::size_t k = 0; k + kRxSampleBytes <= kFramePayload; k += kRxSampleBytes, ++n) {
            put24be(payload + k, 100 * n);                 // I
            put24be(payload + k + 3, -(100 * n + 1));      // Q
        }
    }
    return pkt;
}

int main()
{
    // ---- config register: rate + #RX (+ the openHPSDR-only Mercury/duplex
    //      bits, which HL2 ignores but we still send) ----
    {
        const Cc c = ccConfig(SampleRate::R96k, 1);
        check(c[0] == 0x00, "config C0 is register 0x00");
        check(c[1] == (0x40 | 0x01), "config C1 = Mercury bit | speed(96k=1)");
        check(c[2] == 0x00 && c[3] == 0x00, "config C2/C3 zero");
        check(c[4] == 0x04, "config C4 = duplex, 1 RX");
        check(ccConfig(SampleRate::R48k, 2)[4] == (0x04 | (1 << 3)), "config C4 encodes #RX-1");
        check((c[0] & 0x01) == 0, "config C0 is even (MOX=0, cannot key)");
    }

    // ---- RX1 NCO frequency: 32-bit big-endian ----
    {
        const Cc f = ccRx1Freq(10'000'000);               // 0x00989680
        check(f[0] == 0x04, "rx1freq C0 = 0x04 (RX1 NCO, not TX 0x02)");
        check(f[1] == 0x00 && f[2] == 0x98 && f[3] == 0x96 && f[4] == 0x80, "rx1freq BE bytes");
        check((f[0] & 0x01) == 0, "rx1freq C0 even (MOX=0)");
    }

    // ---- LNA gain: C4 = 0x40 | (dB+12), clamped to [-12,+48] ----
    {
        check(ccRxGain(20)[4] == (0x40 | 32), "gain +20 dB -> code 32");
        check(ccRxGain(-12)[4] == (0x40 | 0), "gain -12 dB -> code 0 (min)");
        check(ccRxGain(48)[4] == (0x40 | 60), "gain +48 dB -> code 60 (max)");
        check(ccRxGain(999)[4] == (0x40 | 60), "gain clamps high");
        check(ccRxGain(-999)[4] == (0x40 | 0), "gain clamps low");
        check(ccRxGain(20)[0] == 0x14, "gain C0 = 0x14 (register 0x0a)");
    }

    // ---- metis command + discovery request ----
    {
        const auto start = metisStart();
        check(start.size() == 64, "metis command padded to 64 B");
        check(start[0] == 0xEF && start[1] == 0xFE && start[2] == 0x04 && start[3] == 0x01,
              "metis start = EF FE 04 01");
        check(metisStop()[3] == 0x00, "metis stop cmd = 0x00");
        const auto disc = discoveryRequest();
        check(disc.size() == 63 && disc[0] == 0xEF && disc[1] == 0xFE && disc[2] == 0x02,
              "discovery request = EF FE 02 + pad");
    }

    // ---- discovery reply parse ----
    {
        std::array<std::uint8_t, 60> reply{};
        reply[0] = 0xEF; reply[1] = 0xFE; reply[2] = 0x02;             // idle
        for (std::size_t i = 0; i < 6; ++i)
            reply[3 + i] = static_cast<std::uint8_t>(0x10 + i);       // MAC
        reply[9] = 0x4A;                                              // gateware
        reply[10] = 0x06;                                            // board id: HL2
        reply[20] = 0x02;                                           // receiver count
        const auto r = parseDiscoveryReply(reply);
        check(r.has_value(), "valid discovery reply parses");
        check(r && r->isHermesLite2(), "board 0x06 -> Hermes-Lite 2");
        check(r && r->mac[0] == 0x10 && r->mac[5] == 0x15, "MAC parsed");
        check(r && r->gatewareVersion == 0x4A, "gateware byte parsed");
        check(r && r->numRx == 0x02, "receiver count (byte 20) parsed");
        check(r && !r->streaming, "status 0x02 -> not streaming");
        reply[2] = 0x03;
        check(parseDiscoveryReply(reply)->streaming, "status 0x03 -> streaming (busy)");
        // Short replies omit byte 20 — numRx must fall back to 0, not read OOB.
        std::array<std::uint8_t, 11> shortReply{};
        shortReply[0] = 0xEF; shortReply[1] = 0xFE; shortReply[2] = 0x02;
        shortReply[10] = 0x06;
        const auto sr = parseDiscoveryReply(shortReply);
        check(sr && sr->numRx == 0, "short reply -> numRx defaults to 0 (no OOB read)");
        std::array<std::uint8_t, 4> junk{0x00, 0x11, 0x22, 0x33};
        check(!parseDiscoveryReply(junk).has_value(), "non-Metis bytes rejected");
    }

    // ---- EP2 packet framing ----
    {
        const auto pkt = ep2Packet(0x01020304, ccConfig(SampleRate::R48k, 1), ccRx1Freq(7'000'000));
        check(pkt.size() == 1032, "EP2 packet is 1032 B");
        check(pkt[0] == 0xEF && pkt[1] == 0xFE && pkt[2] == 0x01 && pkt[3] == 0x02, "EP2 header");
        check(pkt[4] == 0x01 && pkt[5] == 0x02 && pkt[6] == 0x03 && pkt[7] == 0x04, "EP2 seq BE");
        check(pkt[8] == 0x7F && pkt[9] == 0x7F && pkt[10] == 0x7F, "frame A SYNC");
        check(pkt[11] == 0x00, "frame A C&C = config C0");            // ccConfig C0
        check(pkt[8 + 512] == 0x7F && pkt[8 + 512 + 3] == 0x04, "frame B SYNC + rx1freq C0");
    }

    // ---- EP6 decode: seq, 126 samples, exact 24-bit BE IQ + sign-extension ----
    {
        const auto pkt = makeEp6(0xDEADBEEF);
        const auto seq = ep6Seq(pkt);
        check(seq.has_value() && *seq == 0xDEADBEEF, "ep6Seq reads header seq");

        std::vector<std::complex<float>> iq;
        const int n = ep6Samples(pkt, iq);
        check(n == kSamplesPerPacket && iq.size() == 126, "EP6 yields 126 samples (2x63)");
        const float s = 1.0f / static_cast<float>(kFullScale);
        check(approx(iq[0].real(), 0.0f) && approx(iq[0].imag(), -1.0f * s), "sample 0 I/Q");
        check(approx(iq[5].real(), 500.0f * s) && approx(iq[5].imag(), -501.0f * s), "sample 5 I/Q");
        check(approx(iq[125].real(), 12500.0f * s), "last sample I decoded");
    }

    // ---- negative / sign-extension edge: I = -8388608 (24-bit min) ----
    {
        std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
        pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
        pkt[8] = pkt[9] = pkt[10] = 0x7F;                             // frame A SYNC
        pkt[8 + 512] = pkt[9 + 512] = pkt[10 + 512] = 0x7F;          // frame B SYNC
        std::uint8_t* p = pkt.data() + 8 + 8;                         // first sample I
        p[0] = 0x80; p[1] = 0x00; p[2] = 0x00;                        // -2^23
        std::vector<std::complex<float>> iq;
        ep6Samples(pkt, iq);
        check(approx(iq[0].real(), -1.0f), "24-bit min sign-extends to -1.0 full scale");
    }

    // ---- reject non-EP6 packets ----
    {
        std::vector<std::uint8_t> shortPkt(100, 0);
        std::vector<std::complex<float>> scratch;
        check(ep6Samples(shortPkt, scratch) == -1, "short packet rejected");
        std::vector<std::uint8_t> wrongEp(kUsbPacketSize, 0);
        wrongEp[0] = 0xEF; wrongEp[1] = 0xFE; wrongEp[2] = 0x01; wrongEp[3] = 0x02;  // EP2, not EP6
        check(!ep6Seq(wrongEp).has_value(), "EP2 packet not read as EP6");
    }

    // ---- full scale normalises to exactly +-1.0 ----
    //
    // The largest magnitude a 24-bit two's-complement sample can take is
    // 0x7FFFFF = 8388607, so that is the divisor. Dividing by 1<<23 instead
    // leaves full scale reading 0.99999988 and every dBFS figure fractionally
    // low -- tiny, but it is the reference point the S-meter, the spectrum
    // floor, and the clip detector are all quoted against, so it should be the
    // same reference pihpsdr uses rather than one count adrift.
    {
        std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
        pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
        for (const std::size_t fs : {std::size_t(8), std::size_t(8 + kFrameSize)}) {
            pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;
            std::uint8_t* pay = pkt.data() + fs + 8;
            pay[0] = 0x7F; pay[1] = 0xFF; pay[2] = 0xFF;     // I = +full scale
            pay[3] = 0x80; pay[4] = 0x00; pay[5] = 0x00;     // Q = -full scale
        }
        std::vector<std::complex<float>> out;
        check(ep6Samples(pkt, out) == kSamplesPerPacket, "full-scale packet decodes");
        check(out[0].real() == 1.0f, "0x7FFFFF normalises to exactly +1.0");
        // -0x800000 is one count larger in magnitude than +0x7FFFFF, so it maps
        // just past -1.0. That asymmetry is inherent to two's complement.
        check(out[0].imag() < -1.0f && out[0].imag() > -1.0001f,
              "0x800000 normalises to just past -1.0 (two's-complement asymmetry)");
    }

    // ---- one-shot pipeline reset (ENCODER ONLY -- nothing sends this) ----
    //
    // Kept because the byte layout is verified and worth not re-deriving. The
    // radio-facing use of it wedged a board; see MetisProtocol.h kC0Sync.
    {
        const Cc r = ccPipelineReset();
        check(r[0] == kC0Sync, "reset targets addr 0x39 (C0 = 0x72)");
        check((r[0] & 0x01) == 0, "reset bank keeps MOX clear");
        check(r[4] == 0x80, "DATA[7:4] = 0x8 requests a filter-pipeline reset");
        check(r[1] == 0 && r[2] == 0 && r[3] == 0,
              "every other command nibble left at no-action");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_metis_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
