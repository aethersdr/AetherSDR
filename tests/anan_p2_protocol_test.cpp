// aetherd ANAN P2 Phase 1b -- P2Protocol unit test. Pins the openHPSDR
// Protocol 2 wire encoding/decoding ported from the live-validated
// anan/spike/phase1a.py spike, run against a real ANAN-G2 on the bench:
// phase-word conversion, Discovery/General/DDC-Specific/High-Priority packet
// encoding, the strict DDC-frame shape validator, and the 24-bit signed
// big-endian IQ decode. Pure protocol -- no sockets, no hardware.

#include "core/backends/anan/P2Protocol.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace AetherSDR::anan;

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

// Build a well-formed DDC I&Q datagram: 16-byte header + `n` BE 3-byte I/Q
// sample pairs, each sample n at I = 100*n, Q = -(100*n + 1).
static std::vector<std::uint8_t> makeDdcFrame(std::uint32_t seq, int n)
{
    std::vector<std::uint8_t> pkt(static_cast<std::size_t>(kDdcHeaderLen + n * kDdcSampleBytes), 0);
    pkt[0] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[1] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[3] = static_cast<std::uint8_t>(seq & 0xFF);
    // bytes 4-11: timestamp, left zero (unused by this layer)
    pkt[12] = 0x00; pkt[13] = 24;                 // bitsPerSample = 24
    pkt[14] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
    pkt[15] = static_cast<std::uint8_t>(n & 0xFF); // samplesPerFrame = n
    for (int i = 0; i < n; ++i) {
        std::uint8_t* s = pkt.data() + kDdcHeaderLen + i * kDdcSampleBytes;
        put24be(s, 100 * i);
        put24be(s + 3, -(100 * i + 1));
    }
    return pkt;
}

int main()
{
    // ---- phase word: delta = 2^32 * F / Fdsp ----
    {
        check(phaseWord(10'000'000.0) == 0x14D55555u,
              "phaseWord(10 MHz) matches the field-verified value");
        // Round-trip within 1 Hz.
        const double back = static_cast<double>(phaseWord(10'000'000.0))
                           * static_cast<double>(kDspClockHz) / 4294967296.0;
        check(std::fabs(back - 10'000'000.0) < 1.0, "phase word round-trips to within 1 Hz");
        check(phaseWord(0.0) == 0, "0 Hz -> phase word 0");
    }

    // ---- discovery request ----
    {
        const auto disc = buildDiscovery();
        check(disc.size() == 60, "discovery request is 60 bytes");
        check(disc[4] == 0x02, "discovery request byte 4 = 0x02 (Discovery Command)");
        bool restZero = true;
        for (std::size_t i = 0; i < disc.size(); ++i)
            if (i != 4 && disc[i] != 0) { restZero = false; break; }
        check(restZero, "discovery request has no other bytes set");
    }

    // ---- discovery reply parse, against the REAL captured bytes ----
    // anan/reference/notes/captures/hpsdr_discovery/hpsdrDiscovery.txt --
    // ANAN-G2, board type 10 (SATURN), protocol 4.3, firmware/gateware 27,
    // 4 DDCs, phase word, BE+3-byte (byte22==0), p2app build 46.
    {
        const std::array<std::uint8_t, 32> raw{
            0x00, 0x00, 0x00, 0x00, 0x02, 0x88, 0xA2, 0x9E, 0x6D, 0x9F, 0x6A, 0x0A,
            0x2B, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x2E,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        const auto r = parseDiscoveryReply(raw);
        check(r.has_value(), "real captured discovery reply parses");
        check(r && !r->streaming, "byte4=0x02 -> not streaming");
        check(r && r->mac[0] == 0x88 && r->mac[1] == 0xA2 && r->mac[2] == 0x9E
              && r->mac[3] == 0x6D && r->mac[4] == 0x9F && r->mac[5] == 0x6A,
              "MAC parsed (88:A2:9E:6D:9F:6A, matching the capture)");
        check(r && r->boardId == 10, "board id 10 (SATURN / ANAN-G2)");
        check(r && r->isSaturn(), "isSaturn() true for board id 10");
        check(r && r->protoVersionRaw == 0x2B, "proto version raw byte (43 -> 4.3)");
        check(r && r->firmwareVer == 27,
              "firmware is the INTEGER 27, not tenths (discrepancy #1)");
        check(r && r->numDdc == 4, "4 DDCs advertised");
        check(r && r->freqIsPhaseWord, "byte21=1 -> phase word, not Hz");
        check(r && r->endianByteRaw == 0, "byte22 raw is 0");
        check(r && discoveryDeclaresBigEndian3Byte(r->endianByteRaw),
              "byte22==0 decodes as BE+3-byte declared (spec p.45 sentinel), "
              "NOT little-endian -- the hpsdr_discover.py truthiness bug must never "
              "regress into this codec");
        check(r && r->p2appBuild == 46,
              "byte23 is p2app's build number on Saturn, not a beta flag");

        // Bounds-check: a short reply must not be read out of bounds.
        const std::array<std::uint8_t, 10> shortReply{};
        check(!parseDiscoveryReply(shortReply).has_value(),
              "too-short reply rejected, not read OOB");
        // Non-discovery bytes rejected.
        const std::array<std::uint8_t, 24> junk{0x01, 0x02, 0x03, 0x04};
        check(!parseDiscoveryReply(junk).has_value(), "non-zero seq bytes rejected");

        // isSaturn() is false for any other board id -- a picker filter, not
        // a behaviour gate (see the field's own comment in P2Protocol.h).
        auto other = raw;
        other[11] = 5;  // ORION Mk II board id, per spec Appendix A
        const auto ro = parseDiscoveryReply(other);
        check(ro && !ro->isSaturn(), "isSaturn() false for a non-Saturn board id");
    }

    // ---- General packet ----
    {
        const auto g = buildGeneral();
        check(g.size() == 60, "General packet is 60 bytes");
        check(g[4] == 0x00, "General packet command byte = 0x00");
        check(g[37] == 0x08, "byte37 bit3 set: phase word, matching discovery byte21==1");
        check(g[38] == 0x01, "byte38 bit0 set: hardware reset timer / watchdog ON");
        check(g[39] == 0x00,
              "byte39 = 0x00: BE + 3-byte, the only format byte22==0 declares -- "
              "not a guess, confirmed by the 80.0 dB vs 26.0 dB field measurement");
        bool portsZero = true;
        for (std::size_t i = 5; i <= 22; ++i)
            if (g[i] != 0) { portsZero = false; break; }
        check(portsZero, "port table (bytes 5-22) left at defaults");
    }

    // ---- DDC-Specific packet ----
    {
        const auto d = buildDdcSpecific(48);
        check(d.size() == 1444, "DDC-Specific packet is 1444 bytes (full spec length)");
        check(d[4] == 2, "byte4 = 2 ADCs (G2's two phase-synchronous ADCs)");
        check(d[7] == 0x01, "byte7 bit0: DDC0 enabled");
        check(d[17] == 0x00, "byte17: DDC0 -> ADC0");
        check((static_cast<int>(d[18]) << 8 | d[19]) == 48,
              "bytes 18-19: DDC0 rate = 48 (raw ksps, big-endian u16)");
        check(d[22] == 24, "byte22: DDC0 sample size = 24 bits");
        for (std::size_t i = 23; i < 1444; ++i)
            if (d[i] != 0) { check(false, "DDC1-79 fields must stay disabled/zero"); break; }

        const auto d96 = buildDdcSpecific(96, 1);
        check((static_cast<int>(d96[18]) << 8 | d96[19]) == 96, "rate field honours the argument");
        check(d96[4] == 1, "numAdcs argument is honoured");

        // Dither/Random (bytes 5/6) default on, both ADC0 and ADC1 bits
        // together -- one control on this radio, not a per-ADC pair.
        check(d[5] == 0x03 && d[6] == 0x03,
              "dither/random default on, ADC0+ADC1 bits both set");
        const auto dOff = buildDdcSpecific(48, 2, false, false);
        check(dOff[5] == 0x00 && dOff[6] == 0x00, "dither/random both off clears both bytes");

        // ADC select (byte 17): 0 = ADC0 (default, checked above), 1 = ADC1/RX2.
        const auto dAdc1 = buildDdcSpecific(48, 2, true, true, 1);
        check(dAdc1[17] == 0x01, "ddc0AdcIndex=1 routes DDC0 to ADC1/RX2");
    }

    // ---- High Priority: run bit ONLY, no PTT anywhere ----
    {
        const std::uint32_t word = phaseWord(10'000'000.0);
        const auto hp = buildHighPriority(true, word);
        check(hp.size() == 1444, "High Priority packet is 1444 bytes (full spec length)");
        check(hp[4] == 0x01, "run=true -> byte4 = 0x01 (bit0 only)");
        check((static_cast<std::uint32_t>(hp[9]) << 24 | static_cast<std::uint32_t>(hp[10]) << 16
              | static_cast<std::uint32_t>(hp[11]) << 8 | hp[12]) == word,
              "bytes 9-12: DDC0 frequency/phase word round-trips");

        const auto hpOff = buildHighPriority(false, 0);
        check(hpOff[4] == 0x00, "run=false -> byte4 = 0x00");

        // Structural guarantee: byte4 bits 1-4 (PTT0..3) are NEVER set by this
        // encoder, for either run state -- there is no parameter that could
        // set them.
        check((hp[4] & 0x1E) == 0 && (hpOff[4] & 0x1E) == 0,
              "PTT bits (byte4 bits 1-4) are never set -- no parameter exists to set them");

        // Alex0 (bytes 1432-1435): ANT1 (bit 24) always set; HF Bypass
        // (bit 12) tracks bypassAdc0Filters. Alex1 (bytes 1430-1431):
        // HF Bypass 2 (bit 12 of that 16-bit word) tracks bypassAdc1Filters.
        const auto readU32 = [](const std::array<std::uint8_t, 1444>& p, std::size_t i) {
            return (static_cast<std::uint32_t>(p[i]) << 24) | (static_cast<std::uint32_t>(p[i + 1]) << 16)
                 | (static_cast<std::uint32_t>(p[i + 2]) << 8) | p[i + 3];
        };
        const auto readU16 = [](const std::array<std::uint8_t, 1444>& p, std::size_t i) {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[i]) << 8) | p[i + 1]);
        };
        check(readU32(hp, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 12)),
              "default: ANT1 + HF Bypass (ADC0) both set in Alex0");
        check(readU16(hp, 1430) == (std::uint16_t{1} << 12),
              "default: HF Bypass 2 (ADC1/RX2) set in Alex1");

        const auto hpNoBypass = buildHighPriority(true, word, false, false);
        check(readU32(hpNoBypass, 1432) == (std::uint32_t{1} << 24),
              "bypassAdc0Filters=false: ANT1 stays set, HF Bypass clears");
        check(readU16(hpNoBypass, 1430) == 0,
              "bypassAdc1Filters=false: Alex1 stays zero");
    }

    // ---- Multi-DDC encode (DDC-Specific rows at 17+6n, HP words at 9+4n) ----
    // The single-DDC overloads delegate to these, so the first thing to pin
    // is that delegation is byte-identical -- a drift there would silently
    // change the packets the bench-validated DDC0 path already sends.
    {
        const std::array<DdcConfig, 1> justDdc0{DdcConfig{48, 0}};
        check(buildDdcSpecific(justDdc0) == buildDdcSpecific(48),
              "buildDdcSpecific: 1-element list is byte-identical to the single-DDC overload");

        const std::uint32_t word = phaseWord(10'000'000.0);
        const std::array<std::uint32_t, 1> justWord0{word};
        check(buildHighPriority(true, justWord0) == buildHighPriority(true, word),
              "buildHighPriority: 1-element list is byte-identical to the single-DDC overload");
    }

    {
        // Four DDCs, deliberately all different so a row landing on the wrong
        // offset (or every row reading DDC0's values) cannot pass.
        const std::array<DdcConfig, 4> ddcs{
            DdcConfig{48, 0}, DdcConfig{192, 1}, DdcConfig{768, 0}, DdcConfig{1536, 1}};
        const auto d = buildDdcSpecific(ddcs);

        check(d[7] == 0x0F, "byte7 bits 0-3 set: DDC0-3 all enabled");

        bool rowsOk = true;
        for (std::size_t n = 0; n < ddcs.size(); ++n) {
            const std::size_t row = 17 + 6 * n;
            const int rate = static_cast<int>(d[row + 1]) << 8 | d[row + 2];
            if (d[row] != static_cast<std::uint8_t>(ddcs[n].adcIndex)
                || rate != ddcs[n].rateKsps
                || d[row + 3] != 0 || d[row + 4] != 0   // CIC1/CIC2 "For Future use"
                || d[row + 5] != 24) {
                rowsOk = false;
                std::fprintf(stderr,
                    "  DDC%zu row at %zu: adc=%u rate=%d cic=%u,%u size=%u "
                    "(expected adc=%d rate=%d cic=0,0 size=24)\n",
                    n, row, d[row], rate, d[row + 3], d[row + 4], d[row + 5],
                    ddcs[n].adcIndex, ddcs[n].rateKsps);
            }
        }
        check(rowsOk, "each DDC's 6-byte row at 17+6n carries its own ADC/rate/size, "
                      "with CIC1/CIC2 left zero");

        // Nothing beyond the last encoded row: DDC4-79 must stay disabled.
        bool tailZero = true;
        for (std::size_t i = 17 + 6 * ddcs.size(); i < 1444; ++i)
            if (d[i] != 0) { tailZero = false; break; }
        check(tailZero, "DDC4-79 rows stay zero/disabled");

        // High Priority: one 4-byte phase word per DDC at 9+4n.
        const std::array<std::uint32_t, 4> words{
            phaseWord(3'500'000.0), phaseWord(7'100'000.0),
            phaseWord(14'200'000.0), phaseWord(28'400'000.0)};
        const auto hp = buildHighPriority(true, words);
        bool wordsOk = true;
        for (std::size_t n = 0; n < words.size(); ++n) {
            const std::size_t at = 9 + 4 * n;
            const std::uint32_t got =
                static_cast<std::uint32_t>(hp[at]) << 24
              | static_cast<std::uint32_t>(hp[at + 1]) << 16
              | static_cast<std::uint32_t>(hp[at + 2]) << 8
              | hp[at + 3];
            if (got != words[n]) {
                wordsOk = false;
                std::fprintf(stderr, "  DDC%zu phase word at %zu: got %u expected %u\n",
                             n, at, got, words[n]);
            }
        }
        check(wordsOk, "each DDC's frequency/phase word round-trips at 9+4n");

        // Principle VI is structural, not incidental: adding DDCs must not
        // add a way to key. No parameter here can set the PTT bits.
        check((hp[4] & 0x1E) == 0,
              "multi-DDC High Priority still never sets the PTT bits (byte4 bits 1-4)");
    }

    // ---- Sender-port demux (the ONLY thing separating DDC streams) ----
    {
        // Base + n, verified against p2app's generalpacket.c and its
        // DefaultPorts[] table (both give 1035..1044 for DDC0..9).
        check(kDdc0DefaultPort == 1035, "DDC0's default source port is 1035");

        // In range: each port maps to its own index.
        bool mapped = true;
        for (int n = 0; n < 4; ++n) {
            const auto idx = ddcIndexForSenderPort(
                static_cast<std::uint16_t>(1035 + n), 4);
            if (!idx || *idx != n) {
                mapped = false;
                std::fprintf(stderr, "  port %d -> %s (expected %d)\n", 1035 + n,
                             idx ? std::to_string(*idx).c_str() : "nullopt", n);
            }
        }
        check(mapped, "ports 1035..1038 map to DDC 0..3");

        // Below the base, and at/above the active count: both rejected.
        check(!ddcIndexForSenderPort(1034, 4).has_value(),
              "a port below the base is not a DDC stream");
        check(!ddcIndexForSenderPort(1039, 4).has_value(),
              "a port past the active DDC count is rejected (DDC4 with only 4 active)");
        check(ddcIndexForSenderPort(1038, 4).has_value(),
              "the last active DDC's port IS accepted (boundary, not off-by-one)");

        // A single-DDC session must reject DDC1's port -- this is the case
        // that keeps a leftover stream from a previous multi-DDC session
        // from being fed to DDC0.
        check(ddcIndexForSenderPort(1035, 1).has_value(),
              "single-DDC session accepts DDC0's port");
        check(!ddcIndexForSenderPort(1036, 1).has_value(),
              "single-DDC session rejects DDC1's port");

        // Degenerate counts: no DDCs active means nothing is a DDC stream.
        check(!ddcIndexForSenderPort(1035, 0).has_value(),
              "numDdc=0 accepts nothing");
        check(!ddcIndexForSenderPort(1035, -1).has_value(),
              "a negative DDC count accepts nothing rather than underflowing");

        // The shared-socket traffic this must not misclassify: Mic Data and
        // High Priority Status arrive on the same PC-side socket.
        check(!ddcIndexForSenderPort(kRadioPort, 4).has_value(),
              "the radio's command port is never mistaken for a DDC stream");

        // An explicit non-default base still works, for a session that ever
        // negotiates ports via the General packet's bytes 17-18.
        const auto negotiated = ddcIndexForSenderPort(2003, 4, 2000);
        check(negotiated && *negotiated == 3,
              "an explicitly negotiated base port is honoured");
    }

    {
        // Empty list: a legitimate all-disabled packet, not a malformed one.
        const auto none = buildDdcSpecific(std::span<const DdcConfig>{});
        check(none[7] == 0x00, "empty DDC list disables every DDC (byte7 = 0)");
        check(none[4] == 2, "empty DDC list still carries the ADC-count field");

        // Over the cap: extras are ignored rather than encoded or truncating
        // the packet. kMaxDdcs is the codec's own bound (Principle VII).
        const std::array<DdcConfig, 5> tooMany{
            DdcConfig{48, 0}, DdcConfig{48, 0}, DdcConfig{48, 0},
            DdcConfig{48, 0}, DdcConfig{1536, 1}};
        const auto capped = buildDdcSpecific(tooMany);
        check(capped[7] == 0x0F, "more DDCs than kMaxDdcs: only the first 4 are enabled");
        const std::size_t fifthRow = 17 + 6 * 4;
        check(capped[fifthRow] == 0 && capped[fifthRow + 1] == 0
              && capped[fifthRow + 2] == 0 && capped[fifthRow + 5] == 0,
              "the 5th DDC's row is left untouched, not encoded");

        const std::array<std::uint32_t, 5> tooManyWords{1, 2, 3, 4, 0xFFFFFFFFu};
        const auto hpCapped = buildHighPriority(true, tooManyWords);
        const std::size_t fifthWord = 9 + 4 * 4;
        check(hpCapped[fifthWord] == 0 && hpCapped[fifthWord + 1] == 0
              && hpCapped[fifthWord + 2] == 0 && hpCapped[fifthWord + 3] == 0,
              "more phase words than kMaxDdcs: the 5th is ignored, not written");
    }

    // ---- DDC frame: strict shape validation ----
    {
        // A well-formed frame is accepted.
        const auto good = makeDdcFrame(0xDEADBEEF, 2);
        const auto frame = parseDdcFrame(good);
        check(frame.has_value(), "well-formed DDC frame accepted");
        check(frame && frame->seq == 0xDEADBEEF, "sequence number parsed");
        check(frame && frame->samples == 2, "declared sample count parsed");
        check(frame && frame->iqRaw.size() == 2 * static_cast<std::size_t>(kDdcSampleBytes),
              "iqRaw spans exactly samples * kDdcSampleBytes bytes");

        // Too-short datagram (below the 16-byte header) rejected outright.
        const std::array<std::uint8_t, 10> tooShort{};
        check(!parseDdcFrame(tooShort).has_value(), "sub-header-length datagram rejected");

        // Declared/actual length mismatch rejected, NOT clamped -- this is
        // the exact case that misparsed Mic Data as garbage DDC0 frames
        // during Phase 1a before this check existed.
        auto lying = makeDdcFrame(0, 2);
        lying[14] = 0x00; lying[15] = 100;  // declares 100 samples, only 2 present
        check(!parseDdcFrame(lying).has_value(),
              "declared/actual length mismatch rejected, not clamped");

        // Plausible-but-wrong bitsPerSample rejected -- the Mic-Data
        // regression case (a foreign packet that happens to be long enough
        // but isn't a 24-bit DDC frame at all).
        auto wrongBits = makeDdcFrame(0, 2);
        wrongBits[12] = 0x00; wrongBits[13] = 16;  // claims 16-bit samples
        check(!parseDdcFrame(wrongBits).has_value(),
              "non-24-bit bitsPerSample rejected (Mic Data regression case)");

        // An all-zero 60-byte datagram (the shape of a High Priority Status
        // packet) must not be misread as a tiny valid DDC frame.
        const std::array<std::uint8_t, 60> statusShaped{};
        check(!parseDdcFrame(statusShaped).has_value(),
              "a 60-byte all-zero datagram (Status-shaped) is rejected, "
              "not decoded as a garbage DDC frame");
    }

    // ---- IQ sample decode: BE, sign-extension, full scale ----
    {
        const auto good = makeDdcFrame(0, 2);
        const auto frame = parseDdcFrame(good);
        check(frame.has_value(), "frame for decode test parses");
        std::vector<std::complex<float>> iq;
        decodeIq(*frame, iq);
        check(iq.size() == 2, "decodeIq produces exactly `samples` pairs");
        const float s = 1.0f / static_cast<float>(kFullScale24Bit);
        check(approx(iq[0].real(), 0.0f) && approx(iq[0].imag(), -1.0f * s), "sample 0 I/Q");
        check(approx(iq[1].real(), 100.0f * s) && approx(iq[1].imag(), -101.0f * s), "sample 1 I/Q");

        // Full scale and sign-extension edges, mirroring the HL2 test's own
        // full-scale check.
        std::uint8_t fs[6] = {0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00};  // I=+full scale, Q=-full scale
        const auto sample = decodeIqSample(fs);
        check(sample.real() == 1.0f, "0x7FFFFF normalises to exactly +1.0");
        check(sample.imag() < -1.0f && sample.imag() > -1.0001f,
              "0x800000 normalises to just past -1.0 (two's-complement asymmetry)");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_p2_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
