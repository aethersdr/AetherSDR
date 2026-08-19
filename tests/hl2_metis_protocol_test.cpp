// aetherd HL2 Phase 1a — MetisProtocol unit test. Pins the HPSDR Protocol 1
// wire encoding/decoding ported from the live-validated tools/hl2 spike:
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
        const std::size_t rb = ep6RoundBytes(1);
        for (std::size_t k = 0; k + rb <= kFramePayload; k += rb, ++n) {
            put24be(payload + k, 100 * n);                 // I
            put24be(payload + k + 3, -(100 * n + 1));      // Q
        }
    }
    return pkt;
}

// Build a synthetic EP6 packet for `numRx` receivers. Round r of receiver rx
// carries I = 1000*rx + r, Q = -(1000*rx + r) — so a demux that slips by one
// receiver, or that walks the zero padding, produces values from the wrong
// stream rather than something that merely looks plausible.
static std::vector<std::uint8_t> makeEp6Multi(int numRx)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
    const std::size_t rb = ep6RoundBytes(numRx);
    int round = 0;
    for (std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}}) {
        pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;        // SYNC
        std::uint8_t* payload = pkt.data() + fs + 8;
        for (std::size_t k = 0; k + rb <= kFramePayload; k += rb, ++round) {
            for (int rx = 0; rx < numRx; ++rx) {
                std::uint8_t* s = payload + k + static_cast<std::size_t>(rx) * kRxIqBytes;
                put24be(s, 1000 * rx + round);
                put24be(s + 3, -(1000 * rx + round));
            }
            // The 2 mic bytes closing the round get a non-zero marker: if the
            // decoder ever mistook them for IQ the values would show up.
            payload[k + rb - 2] = 0x5A;
            payload[k + rb - 1] = 0xA5;
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

    // ---- J16 open-collector filter byte: DATA[23:17] == C2[7:1] ----
    //
    // The SHIFT is the whole point. DATA[16] is not part of the field, so an
    // unshifted byte would put every selection one relay too low — 80 m would
    // engage the 160 m filter. Asserted on the encoder, not inferred.
    {
        const Cc c = ccConfig(SampleRate::R48k, 1, kOcLpf160);
        check(c[2] == 0x02, "oc 0x01 (160m) lands in C2 bit 1, not bit 0");
        check(ccConfig(SampleRate::R48k, 1, kOcHpfAmBc | kOcLpf12_10)[2] == 0xC0,
              "oc 0x60 (HPF|12/10m) -> C2 0xC0");
        // Bit 7 is the RX-antenna bit and lives at DATA[13], not in this field.
        // Passing a full I2C byte must not shift it into DATA[24] (the sample
        // rate's low bit lives at [24] — this would change the DDC rate).
        check(ccConfig(SampleRate::R48k, 1, 0xFF)[2] == 0xFE,
              "oc bit 7 masked off (it is the RX antenna bit, not a filter)");
        check(ccConfig(SampleRate::R48k, 1, kOcNone)[2] == 0x00,
              "oc none releases every relay");
        // The filter byte must not disturb what shares this register.
        check(c[1] == (0x40 | 0x00) && c[4] == 0x04,
              "oc byte leaves sample rate and #RX untouched");
    }

    // ---- band -> filter selection ----
    //
    // Every value below is Quisk's Hermes_BandDict for that band
    // (quisk_conf_defaults.py), which is the reference client's own table for
    // the N2ADR companion board. Checking against it is what makes the
    // frequency RANGES in ocFilterByteForHz() verifiable rather than plausible:
    // the ranges are ours, the answers are not.
    {
        struct { double mhz; std::uint8_t oc; const char* what; } cases[] = {
            {  1.900, 0b0000001, "160m -> 160 LPF, HPF out" },
            {  3.800, 0b1000010, "80m  -> HPF + 80 LPF" },
            {  5.357, 0b1000100, "60m  -> HPF + 60/40 LPF" },
            {  7.200, 0b1000100, "40m  -> HPF + 60/40 LPF" },
            { 10.125, 0b1001000, "30m  -> HPF + 30/20 LPF" },
            { 14.225, 0b1001000, "20m  -> HPF + 30/20 LPF" },
            { 18.130, 0b1010000, "17m  -> HPF + 17/15 LPF" },
            { 21.300, 0b1010000, "15m  -> HPF + 17/15 LPF" },
            { 24.950, 0b1100000, "12m  -> HPF + 12/10 LPF" },
            { 28.400, 0b1100000, "10m  -> HPF + 12/10 LPF" },
        };
        for (const auto& c : cases)
            check(ocFilterByteForHz(c.mhz * 1.0e6) == c.oc, c.what);

        // Band EDGES, not just the button's default frequency: a range boundary
        // that fell inside a band would filter one end of it correctly and the
        // other end through the neighbouring low-pass.
        check(ocFilterByteForHz(1.800e6) == ocFilterByteForHz(2.000e6),
              "160m band edges share one filter");
        check(ocFilterByteForHz(3.500e6) == ocFilterByteForHz(4.000e6),
              "80m band edges share one filter");
        check(ocFilterByteForHz(7.000e6) == ocFilterByteForHz(7.300e6),
              "40m band edges share one filter");
        check(ocFilterByteForHz(14.000e6) == ocFilterByteForHz(14.350e6),
              "20m band edges share one filter");
        check(ocFilterByteForHz(28.000e6) == ocFilterByteForHz(29.700e6),
              "10m band edges share one filter");

        // Outside the board's coverage nothing is engaged — a low-pass chosen
        // for a band that is not there would only attenuate.
        check(ocFilterByteForHz(0.600e6) == kOcNone,
              "AM broadcast: no filter, and specifically not the AM-blocking HPF");
        check(ocFilterByteForHz(50.150e6) == kOcNone, "6m: no filter fitted");
        // Never the RX-antenna bit, whatever the frequency.
        check((ocFilterByteForHz(14.2e6) & 0x80) == 0, "filter byte never sets bit 7");
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
        // Receiver count is at offset 0x13 = 19. The two neighbours are filled
        // with DIFFERENT, plausible values on purpose: this assertion used to
        // write the count at 20 and read it back from 20, so it agreed with an
        // implementation that was off by one and could never have caught it.
        //
        // Offset 20 is {BANDSCOPE_BITS, BOARD[5:0]} — 0x45 is a build-5 board
        // with the wideband bits set, which is exactly the kind of value that
        // reads as a believable receiver count.
        reply[19] = 0x04;                                          // receiver count (0x13)
        reply[20] = 0x45;                                          // NOT the count
        reply[18] = 0x77;                                          // NOT the count
        const auto r = parseDiscoveryReply(reply);
        check(r.has_value(), "valid discovery reply parses");
        check(r && r->isHermesLite2(), "board 0x06 -> Hermes-Lite 2");
        check(r && r->mac[0] == 0x10 && r->mac[5] == 0x15, "MAC parsed");
        check(r && r->gatewareVersion == 0x4A, "gateware byte parsed");
        check(r && r->numRx == 0x04, "receiver count read from offset 0x13 (19)");
        check(r && r->numRx != 0x45, "not offset 20 — that is the board/bandscope byte");
        check(r && r->numRx != 0x77, "not offset 18");
        check(r && !r->streaming, "status 0x02 -> not streaming");
        reply[2] = 0x03;
        check(parseDiscoveryReply(reply)->streaming, "status 0x03 -> streaming (busy)");
        // A reply that stops before offset 19 must fall back to 0, not read OOB.
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
        check(n == ep6SamplesPerPacket(1) && iq.size() == 126, "EP6 yields 126 samples (2x63)");
        const float s = 1.0f / static_cast<float>(kFullScale);
        check(approx(iq[0].real(), 0.0f) && approx(iq[0].imag(), -1.0f * s), "sample 0 I/Q");
        check(approx(iq[5].real(), 500.0f * s) && approx(iq[5].imag(), -501.0f * s), "sample 5 I/Q");
        check(approx(iq[125].real(), 12500.0f * s), "last sample I decoded");
    }

    // ---- EP6 payload geometry at N receivers ----
    //
    // These numbers come from the gateware, not from us: usopenhpsdr1.v emits
    // whole rounds while another fits and then zero-PADS the rest of the frame
    // (MIC0 -> `(byte_no[8:0] > round_bytes) ? RXDATA2 : PAD`). So the count is
    // a floor division, and only N=1 and N=2 happen to divide 504 exactly.
    // Getting this wrong does not fail loudly — it decodes padding as samples.
    {
        check(ep6RoundBytes(1) == 8 && ep6RoundBytes(2) == 14
              && ep6RoundBytes(3) == 20 && ep6RoundBytes(4) == 26,
              "round is 6*numRx + 2 bytes");
        check(ep6RoundsPerFrame(1) == 63, "1 RX: 63 rounds, 504 bytes, no padding");
        check(ep6RoundsPerFrame(2) == 36, "2 RX: 36 rounds, 504 bytes, no padding");
        check(ep6RoundsPerFrame(3) == 25, "3 RX: 25 rounds, 500 bytes, 4 padding");
        check(ep6RoundsPerFrame(4) == 19, "4 RX: 19 rounds, 494 bytes, 10 padding");
        check(ep6SamplesPerPacket(4) == 38, "4 RX: 38 samples per receiver per packet");
        // The per-receiver sample count FALLS as receivers are added; a caller
        // that keeps treating 126 as a constant overruns its own buffers.
        check(ep6SamplesPerPacket(4) < ep6SamplesPerPacket(1),
              "samples per receiver fall as receivers are added");
        // numRx = 0 must not divide by zero or return something enormous.
        check(ep6RoundBytes(0) == ep6RoundBytes(1), "numRx < 1 clamps to 1");
    }

    // ---- link budget: which (rate, receiver-count) pairs are deliverable ----
    //
    // The HL2's ethernet is 100BASE-T and BOTH axes cost bandwidth: adding a
    // receiver shrinks the per-receiver payload of a fixed-size packet, so the
    // radio sends more packets. Four receivers at 384 kHz is ~89 Mbit/s of wire
    // traffic, which does not fail cleanly — it drops, and a dropped EP6 packet
    // is a gap in every panadapter at once.
    {
        check(ep6BitsPerSecond(48000, 1) < 4.0e6, "1 RX at 48k is ~3.3 Mbit/s");
        check(ep6BitsPerSecond(384000, 4) > 85.0e6, "4 RX at 384k exceeds 85 Mbit/s");
        check(ep6BitsPerSecond(384000, 4) > ep6BitsPerSecond(384000, 1),
              "receivers cost bandwidth even at a fixed sample rate");

        check(maxReceiversAtRate(48000, 4) == 4, "48k carries 4 receivers");
        check(maxReceiversAtRate(96000, 4) == 4, "96k carries 4 receivers");
        check(maxReceiversAtRate(192000, 4) == 4, "192k carries 4 receivers");
        check(maxReceiversAtRate(384000, 4) == 3, "384k carries only 3 receivers");
        check(maxReceiversAtRate(384000, 1) == 1, "hardMax is respected");
        // Never returns zero: a rate too fast for even one receiver must still
        // give a runnable radio rather than a configuration with no receivers.
        check(maxReceiversAtRate(10'000'000, 4) >= 1, "always admits at least one receiver");
    }

    // ---- EP6 demux across N receivers ----
    {
        for (const int numRx : {1, 2, 3, 4}) {
            const auto pkt = makeEp6Multi(numRx);
            std::vector<std::vector<std::complex<float>>> out(static_cast<std::size_t>(numRx));
            const int n = ep6SamplesMulti(pkt, out);
            check(n == ep6SamplesPerPacket(numRx), "demux returns samples per receiver");

            const float s = 1.0f / static_cast<float>(kFullScale);
            bool allOk = true;
            for (int rx = 0; rx < numRx && allOk; ++rx) {
                const auto& v = out[static_cast<std::size_t>(rx)];
                if (static_cast<int>(v.size()) != n) { allOk = false; break; }
                for (int r = 0; r < n; ++r) {
                    // Round index restarts per frame in the builder's `round`
                    // counter only across the whole packet, so expected value is
                    // simply the running round number.
                    const float want = static_cast<float>(1000 * rx + r) * s;
                    if (!approx(v[static_cast<std::size_t>(r)].real(), want)
                        || !approx(v[static_cast<std::size_t>(r)].imag(), -want)) {
                        allOk = false;
                        break;
                    }
                }
            }
            check(allOk, "every receiver's samples decode to its own stream");
        }
    }

    // ---- demux refuses a receiver count it cannot honour ----
    {
        const auto pkt = makeEp6Multi(2);
        std::vector<std::vector<std::complex<float>>> none;
        check(ep6SamplesMulti(pkt, none) == -1, "empty output span is rejected");
        std::vector<std::vector<std::complex<float>>> tooMany(kMaxReceivers + 1);
        check(ep6SamplesMulti(pkt, tooMany) == -1, "more than kMaxReceivers is rejected");
    }

    // ---- per-receiver NCO registers ----
    //
    // RX1..RX7 are registers 0x02..0x08, and C0 is the address shifted left one
    // because C0 bit 0 is MOX. An encoder that forgot the shift would write the
    // TX NCO (0x01) when asked for RX1.
    {
        check(ccRxFreq(0, 7'200'000)[0] == kC0Rx1Freq, "RX1 -> C0 0x04 (addr 0x02)");
        check(ccRxFreq(1, 7'200'000)[0] == 0x06, "RX2 -> C0 0x06 (addr 0x03)");
        check(ccRxFreq(2, 7'200'000)[0] == 0x08, "RX3 -> C0 0x08 (addr 0x04)");
        check(ccRxFreq(3, 7'200'000)[0] == 0x0A, "RX4 -> C0 0x0A (addr 0x05)");
        check(ccRxFreq(6, 7'200'000)[0] == 0x10, "RX7 -> C0 0x10 (addr 0x08)");
        for (int rx = 0; rx <= 6; ++rx) {
            check((ccRxFreq(rx, 7'200'000)[0] & 0x01) == 0,
                  "every RX NCO C0 is even (MOX=0, cannot key)");
            check(ccRxFreq(rx, 7'200'000)[0] != kC0TxFreq,
                  "no RX NCO collides with the TX NCO register");
        }
        // The payload is plain Hz, big-endian, and identical across receivers.
        const Cc a = ccRxFreq(0, 14'074'000);
        const Cc b = ccRxFreq(3, 14'074'000);
        check(a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4],
              "frequency payload does not depend on the receiver index");
        check(ccRx1Freq(14'074'000)[0] == a[0] && ccRx1Freq(14'074'000)[4] == a[4],
              "ccRx1Freq is ccRxFreq(0, ...)");
        // Out of range clamps into the encodable run rather than walking off it.
        check(ccRxFreq(99, 1)[0] == ccRxFreq(6, 1)[0], "rxIndex clamps at RX7");
        check(ccRxFreq(-1, 1)[0] == ccRxFreq(0, 1)[0], "negative rxIndex clamps at RX1");
    }

    // ---- receiver count is a FOUR-bit field at DATA[6:3] ----
    {
        check(ccConfig(SampleRate::R48k, 4)[4] == (0x04 | (3 << 3)), "4 RX encodes as 3");
        check(ccConfig(SampleRate::R48k, 8)[4] == (0x04 | (7 << 3)), "8 RX encodes as 7");
        // The old 3-bit mask wrapped here: 9 RX (code 8) became code 0, i.e. ONE
        // receiver, and the radio would have streamed a layout nobody expected.
        check(ccConfig(SampleRate::R48k, 9)[4] == (0x04 | (8 << 3)), "9 RX encodes as 8, not 0");
        check(ccConfig(SampleRate::R48k, 12)[4] == (0x04 | (11 << 3)), "12 RX encodes as 11");
        check(ccConfig(SampleRate::R48k, 99)[4] == (0x04 | (11 << 3)), "over-max clamps to 12");
        check(ccConfig(SampleRate::R48k, 0)[4] == 0x04, "under-min clamps to 1");
        // The count must not bleed into the sample rate, which shares C1/C4.
        check(ccConfig(SampleRate::R384k, 12)[1] == (0x40 | 0x03),
              "receiver count leaves the sample rate untouched");
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
        check(ep6Samples(pkt, out) == ep6SamplesPerPacket(1), "full-scale packet decodes");
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

    // ---- TX encoders ----
    {
        const Cc f = ccTxFreq(14'200'000);
        check(f[0] == kC0TxFreq, "TX freq targets addr 0x01 (C0 = 0x02)");
        check(f[1] == 0x00 && f[2] == 0xD8 && f[3] == 0xAC && f[4] == 0xC0,
              "TX freq is 32-bit big-endian Hz (14.2 MHz = 0x00D8ACC0)");
        check((f[0] & kC0MoxBit) == 0, "TX freq bank is not keyed by itself");

        const Cc d = ccTxDrive(200);
        check(d[0] == kC0TxDrive, "TX drive targets addr 0x09 (C0 = 0x12)");
        check(d[1] == 200, "drive level lands in C1 (DATA[31:24])");
        check(d[2] == 0 && d[3] == 0 && d[4] == 0,
              "PA stays OFF unless explicitly asked for");
        const Cc dpa = ccTxDrive(200, true);
        check(dpa[2] == 0x08, "PA enable is DATA[19] = C2 bit 3");
        check(dpa[3] == 0 && dpa[4] == 0,
              "enabling the PA does not set ATU tune or Alex bits");
        check(ccTxDrive(-5)[1] == 0 && ccTxDrive(9999)[1] == kTxDriveMax,
              "drive level clamps to 0..255");
    }

    // ---- MOX is a property of the FRAME, not a register ----
    {
        const Cc keyed = withMox(ccRx1Freq(7'000'000), true);
        check((keyed[0] & kC0MoxBit) != 0, "withMox sets C0 bit 0");
        check((keyed[0] & ~kC0MoxBit) == kC0Rx1Freq,
              "withMox leaves the register address intact");
        check((withMox(keyed, false)[0] & kC0MoxBit) == 0, "withMox clears again");
        // Any bank can carry MOX -- the radio reads it from whatever is in flight.
        check((withMox(ccConfig(SampleRate::R48k, 1), true)[0] & kC0MoxBit) != 0,
              "the config bank can carry MOX too");
    }

    // ---- TX IQ payload, and the EADDR trap ----
    {
        auto pkt = ep2Packet(0, ccConfig(SampleRate::R48k, 1), ccRx1Freq(7'000'000));
        std::vector<std::complex<float>> iq;
        iq.emplace_back(1.0f, -1.0f);            // full scale both rails
        iq.emplace_back(0.0f, 0.5f);
        ep2WriteTxIq(pkt, iq);

        const std::uint8_t* pay = pkt.data() + 8 + 8;    // frame 0, after SYNC + C&C
        // Sample 0: I = +32767, Q = -32767.
        check(pay[4] == 0x7F && pay[5] == 0xFF, "full-scale I clamps to +32767, big-endian");
        check(pay[6] == 0x80 && pay[7] == 0x01,
              "full-scale -1.0 clamps to -32767, NOT -32768 (no wrap to the far rail)");

        // THE TRAP: the first 32-bit word of each frame's payload is EADDR, the
        // extended-address register -- not headphone audio. A memcpy'd Hermes TX
        // layout writes garbage there. It must stay zero.
        check(pay[0] == 0 && pay[1] == 0 && pay[2] == 0 && pay[3] == 0,
              "EADDR (first 32-bit word after C&C) left zero");
        const std::uint8_t* pay1 = pkt.data() + 8 + kFrameSize + 8;
        check(pay1[0] == 0 && pay1[1] == 0 && pay1[2] == 0 && pay1[3] == 0,
              "second frame's EADDR left zero as well");

        // Sample 1 lands in the next 8-byte slot; the rest is transmit silence.
        check(pay[12] == 0x00 && pay[13] == 0x00, "sample 1 I = 0");
        check(pay[14] == 0x3F && pay[15] == 0xFF, "sample 1 Q = 0.5 -> 16383");
        check(pay[20] == 0 && pay[21] == 0 && pay[22] == 0 && pay[23] == 0,
              "unsupplied samples are transmit silence");
    }

    // ---- EP6 C&C response decoding (telemetry) ----
    {
        auto frame = [](std::uint8_t c0, std::uint32_t data) {
            std::array<std::uint8_t, 8> f{};
            f[0] = f[1] = f[2] = 0x7F;
            f[3] = c0;
            f[4] = static_cast<std::uint8_t>((data >> 24) & 0xFF);
            f[5] = static_cast<std::uint8_t>((data >> 16) & 0xFF);
            f[6] = static_cast<std::uint8_t>((data >> 8) & 0xFF);
            f[7] = static_cast<std::uint8_t>(data & 0xFF);
            return f;
        };

        // Classic cycle: C0 = 0, 8, 16 -> RADDR 0, 1, 2 (hpsdrsim's own sequence).
        auto r0 = parseEp6Response(frame(0x00, 0).data());
        check(r0.has_value() && !r0->ack && r0->raddr == 0, "C0=0x00 -> RADDR 0, ACK clear");
        auto r1 = parseEp6Response(frame(0x08, 0).data());
        check(r1.has_value() && r1->raddr == 1, "C0=0x08 -> RADDR 1");
        auto r2 = parseEp6Response(frame(0x10, 0).data());
        check(r2.has_value() && r2->raddr == 2, "C0=0x10 -> RADDR 2");

        // ACK changes how C0 is read: 6 address bits instead of 4.
        auto ra = parseEp6Response(frame(0x80 | (0x2A << 1), 0).data());
        check(ra.has_value() && ra->ack && ra->raddr == 0x2A,
              "ACK=1 -> RADDR is C0[6:1], all six bits");

        // PTT and Dot ride in C0 regardless.
        auto rp = parseEp6Response(frame(0x08 | 0x01, 0).data());
        check(rp.has_value() && rp->ptt, "PTT decoded from C0[0]");
        auto rd = parseEp6Response(frame(0x08 | 0x04, 0).data());
        check(rd.has_value() && rd->dot, "Dot decoded from C0[2]");

        check(!parseEp6Response(std::array<std::uint8_t,8>{}.data()).has_value(),
              "unsynced frame rejected");

        Hl2Telemetry t;
        check(!t.forwardPowerRaw.has_value(), "telemetry starts unknown, not zero");

        // RADDR 0: firmware 0x15, ADC overload set, TX PERMITTED (bit 25 high).
        t.apply(*parseEp6Response(frame(0x00, (1u<<25) | (1u<<24) | 0x15).data()));
        check(t.firmwareVersion.value_or(-1) == 0x15, "firmware version from DATA[7:0]");
        check(t.adcOverload.value_or(false), "ADC overload from bit 24");
        check(t.txInhibited.value_or(true) == false,
              "bit 25 SET means transmit permitted (active low, inverted here)");
        t.apply(*parseEp6Response(frame(0x00, 0).data()));
        check(t.txInhibited.value_or(false) == true, "bit 25 clear means INHIBITED");

        // RADDR 1: temperature high half, forward power low half.
        t.apply(*parseEp6Response(frame(0x08, (1234u << 16) | 5678u).data()));
        check(t.temperatureRaw.value_or(-1) == 1234, "temperature from DATA[31:16]");
        check(t.forwardPowerRaw.value_or(-1) == 5678, "forward power from DATA[15:0]");

        // RADDR 2: reverse power and bias.
        t.apply(*parseEp6Response(frame(0x10, (100u << 16) | 42u).data()));
        check(t.reversePowerRaw.value_or(-1) == 100, "reverse power from DATA[31:16]");
        check(t.biasCurrentRaw.value_or(-1) == 42, "bias current from DATA[15:0]");
    }

    // ---- SWR ----
    {
        check(!swrFromRaw(0, 0).has_value(),
              "no forward power -> SWR is unknown, NOT 1.0");
        const auto flat = swrFromRaw(1000, 0);
        check(flat.has_value() && std::fabs(*flat - 1.0) < 1e-9,
              "no reflection -> 1.0:1");
        // Voltage form: rho = 1/3 -> SWR 2.0. The power form would have given
        // 1.0/(1-sqrt(1/3)) ~= 2.37 here, i.e. a different and wrong number.
        const auto two = swrFromRaw(3000, 1000);
        check(two.has_value() && std::fabs(*two - 2.0) < 1e-9,
              "rho = 1/3 -> 2.0:1 (voltage form, no square root)");
        const auto bad = swrFromRaw(100, 500);
        check(bad.has_value() && *bad > 100.0,
              "reverse above forward clamps to a very high SWR, not negative");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_metis_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
