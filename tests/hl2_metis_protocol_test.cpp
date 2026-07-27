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
