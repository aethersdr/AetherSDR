// aetherd ANAN P2 Phase 1b -- P2Protocol unit test. Pins the openHPSDR
// Protocol 2 wire encoding/decoding ported from the live-validated
// anan/spike/phase1a.py spike, run against a real ANAN-G2 on the bench:
// phase-word conversion, Discovery/General/DDC-Specific/High-Priority packet
// encoding, the strict DDC-frame shape validator, and the 24-bit signed
// big-endian IQ decode. Pure protocol -- no sockets, no hardware.

#include "core/backends/anan/P2Protocol.h"
#include "core/backends/anan/AnanSliceAudio.h"
#include "core/backends/anan/AnanSpeakerPacing.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
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

        // Step attenuators (pp.34,36): byte 1443 = ADC0, byte 1442 = ADC1,
        // 0-31 dB. Default is none; values land on the right ADC's byte and
        // are clamped to the spec range at both ends.
        check(hp[1443] == 0 && hp[1442] == 0,
              "default: both step attenuators at 0 dB");
        const auto hpAtt = buildHighPriority(true, word, true, true, 12, 20);
        check(hpAtt[1443] == 12, "adc0AttenuationDb=12 -> byte 1443 = 12");
        check(hpAtt[1442] == 20, "adc1AttenuationDb=20 -> byte 1442 = 20");
        const auto hpClamp = buildHighPriority(true, word, true, true, 40, -5);
        check(hpClamp[1443] == 31, "attenuation above 31 dB clamps to 31");
        check(hpClamp[1442] == 0, "negative attenuation clamps to 0");
        check(readU32(hpAtt, 1432) == readU32(hp, 1432)
                  && readU16(hpAtt, 1430) == readU16(hp, 1430)
                  && hpAtt[4] == hp[4],
              "attenuation touches no other field (Alex0/Alex1/run unchanged)");
        const std::array<std::uint32_t, 1> oneWord{word};
        check(buildHighPriority(true, oneWord, true, true, 12, 20) == hpAtt,
              "multi-DDC overload carries the attenuators identically");
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

    // ---- DDC Audio, PC -> radio: buildSpeakerAudio ----
    {
        std::vector<std::int16_t> full(kSpeakerFramesPerPacket * kSpeakerChannels);
        for (std::size_t i = 0; i < full.size(); ++i) {
            full[i] = static_cast<std::int16_t>(i);
        }
        const auto pkt = buildSpeakerAudio(0x01020304u, full);
        check(pkt.size() == kSpeakerPacketBytes, "speaker packet is exactly 260 bytes");
        check(pkt[0] == 0x01 && pkt[1] == 0x02 && pkt[2] == 0x03 && pkt[3] == 0x04,
              "sequence is big-endian in bytes 0-3");
        // Sample 1 is the value 1, so its two bytes are 0x00,0x01 -- high byte
        // first. A little-endian encoder passes a symmetric-value test and fails
        // this one, which is the whole point of using an asymmetric value.
        check(pkt[4] == 0x00 && pkt[5] == 0x00, "sample 0 == 0");
        check(pkt[6] == 0x00 && pkt[7] == 0x01, "sample 1 is big-endian 0x0001");
        check(pkt[8] == 0x00 && pkt[9] == 0x02, "sample 2 is big-endian 0x0002");
        // Last sample is index 127 == 0x007F, at bytes 258-259.
        check(pkt[258] == 0x00 && pkt[259] == 0x7F, "the 128th sample lands in the last two bytes");

        // Negative samples: two's complement on the wire, not a sign-shifted
        // value. -2 is 0xFFFE; an encoder that shifted the signed type right
        // would be implementation-defined here.
        const std::array<std::int16_t, 4> neg{-1, -2, -32768, 32767};
        const auto negPkt = buildSpeakerAudio(0, neg);
        check(negPkt[4] == 0xFF && negPkt[5] == 0xFF, "-1 encodes as 0xFFFF");
        check(negPkt[6] == 0xFF && negPkt[7] == 0xFE, "-2 encodes as 0xFFFE");
        check(negPkt[8] == 0x80 && negPkt[9] == 0x00, "-32768 encodes as 0x8000");
        check(negPkt[10] == 0x7F && negPkt[11] == 0xFF, "+32767 encodes as 0x7FFF");
        // Everything past a short input is silence, not stale content.
        bool tailSilent = true;
        for (std::size_t i = 12; i < kSpeakerPacketBytes; ++i) {
            if (negPkt[i] != 0) tailSilent = false;
        }
        check(tailSilent, "a short input zero-fills the rest of the packet");

        // Excess is ignored rather than overrunning the packet.
        std::vector<std::int16_t> tooMany(kSpeakerFramesPerPacket * kSpeakerChannels + 64, 9);
        const auto clipped = buildSpeakerAudio(7, tooMany);
        check(clipped.size() == kSpeakerPacketBytes, "excess input still yields one packet");
        check(clipped[258] == 0x00 && clipped[259] == 0x09, "the packet fills to its last byte");
    }

    // ---- High Priority Status: the two speaker fields ----
    {
        std::vector<std::uint8_t> pkt(kHighPriorityStatusBytes, 0);
        pkt[0] = 0x00; pkt[1] = 0x00; pkt[2] = 0x01; pkt[3] = 0x02;  // seq 258
        pkt[30] = 0b0000'1000;            // speaker underflow, bit 3
        pkt[37] = 0x01; pkt[38] = 0x40;   // level 320, big-endian

        const auto st = parseHighPriorityStatus(pkt);
        check(st.has_value(), "a 60-byte status packet parses");
        if (st) {
            check(st->seq == 258, "the sequence is big-endian");
            check(st->speakerUnderflow, "byte 30 bit 3 is the speaker underflow");
            check(st->speakerFifoLevel == 320, "bytes 37-38 are the level, big-endian");
        }

        // Bit 3 only. Bit 2 is the DUC's underflow and must not read as ours --
        // that is the mistake the bitmask exists to prevent.
        pkt[30] = 0b0000'0100;
        const auto duc = parseHighPriorityStatus(pkt);
        check(duc.has_value() && !duc->speakerUnderflow,
              "the DUC's underflow bit is not read as the speaker's");
        pkt[30] = 0b1111'0111;   // every bit BUT ours
        const auto others = parseHighPriorityStatus(pkt);
        check(others.has_value() && !others->speakerUnderflow,
              "no other overflow bit reads as the speaker's");
        pkt[30] = 0b1111'1111;
        const auto all = parseHighPriorityStatus(pkt);
        check(all.has_value() && all->speakerUnderflow,
              "our bit is still seen when every other one is set too");

        // Exact length, like parseDdcFrame: a different length is a different
        // packet, not a truncated status. Bounds are checked before indexing, so a
        // short buffer must not be read past (Principle VII).
        std::vector<std::uint8_t> shortPkt(kHighPriorityStatusBytes - 1, 0);
        check(!parseHighPriorityStatus(shortPkt).has_value(),
              "a 59-byte datagram is not a status packet");
        std::vector<std::uint8_t> longPkt(kHighPriorityStatusBytes + 1, 0);
        check(!parseHighPriorityStatus(longPkt).has_value(),
              "a 61-byte datagram is not a status packet");
        check(!parseHighPriorityStatus(std::span<const std::uint8_t>{}).has_value(),
              "an empty datagram is not a status packet");

        // A Discovery reply is also 60 bytes. It parses as BOTH, which is why the
        // caller must try the reply first -- pinned here so the ordering
        // requirement is visible from the test, not only from a comment.
        std::vector<std::uint8_t> reply(kHighPriorityStatusBytes, 0);
        reply[4] = 0x02;
        check(parseDiscoveryReply(reply).has_value()
              && parseHighPriorityStatus(reply).has_value(),
              "a discovery reply satisfies both parsers, so reply-first ordering matters");
    }

    // ---- the receiver's audio stage ----
    {
        check(sliceAudioAmplitude(100) == 1.0f, "gain 100 is unity");
        check(sliceAudioAmplitude(0) == 0.0f, "gain 0 is exactly silent, not -40 dB");
        check(sliceAudioAmplitude(150) == 1.0f, "gain above 100 clamps to unity");
        check(sliceAudioAmplitude(-5) == 0.0f, "gain below 0 clamps to silence");
        // Midpoint is -20 dB by construction: 10^(0.05 * -20) == 0.1.
        check(approx(sliceAudioAmplitude(50), 0.1f), "gain 50 is -20 dB");
        // Monotonic, and NOT linear -- a linear law would put 50 at 0.5. This is
        // the check that fails if the dB curve is replaced by a linear one.
        check(sliceAudioAmplitude(50) < 0.2f, "the law is dB, not linear");
        check(sliceAudioAmplitude(25) < sliceAudioAmplitude(50)
              && sliceAudioAmplitude(50) < sliceAudioAmplitude(75),
              "gain is monotonic across its range");

        // Balance attenuates one side and never boosts the other.
        check(sliceAudioLeftPanGain(50) == 1.0f && sliceAudioRightPanGain(50) == 1.0f,
              "centred balance leaves both channels alone");
        check(sliceAudioLeftPanGain(0) == 1.0f && sliceAudioRightPanGain(0) == 0.0f,
              "hard left silences the right channel");
        check(sliceAudioRightPanGain(100) == 1.0f && sliceAudioLeftPanGain(100) == 0.0f,
              "hard right silences the left channel");
        check(sliceAudioLeftPanGain(75) <= 1.0f && sliceAudioRightPanGain(75) <= 1.0f,
              "no balance setting boosts either channel above unity");

        // Applied to a real block.
        float block[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        applySliceAudioInPlace(block, 4, true, 100, 50);
        bool muted = true;
        for (float v : block) { if (v != 0.0f) muted = false; }
        check(muted, "mute zeroes every sample in the block");

        float panned[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        applySliceAudioInPlace(panned, 2, false, 100, 0);
        check(panned[0] == 1.0f && panned[1] == 0.0f && panned[2] == 1.0f && panned[3] == 0.0f,
              "hard-left balance keeps L and silences R, frame by frame");

        float scaled[2] = {1.0f, 1.0f};
        applySliceAudioInPlace(scaled, 1, false, 50, 50);
        check(approx(scaled[0], 0.1f) && approx(scaled[1], 0.1f),
              "gain 50 scales both channels by -20 dB");

        // Mute wins over a live gain, and does not consume it.
        float both[2] = {1.0f, 1.0f};
        applySliceAudioInPlace(both, 1, true, 50, 0);
        check(both[0] == 0.0f && both[1] == 0.0f, "mute wins over gain and balance");

        // Quantising to the wire format. NaN is the case a clamp cannot catch;
        // infinity pins the predicate as isfinite rather than isnan.
        check(sliceAudioToInt16(0.0f) == 0, "silence quantises to zero");
        check(sliceAudioToInt16(1.0f) == 32767, "full scale is the positive peak");
        check(sliceAudioToInt16(-1.0f) == -32767, "negative full scale mirrors it");
        check(sliceAudioToInt16(2.0f) == 32767, "an overshoot clamps, not wraps");
        check(sliceAudioToInt16(-2.0f) == -32767, "and so does a negative overshoot");
        check(sliceAudioToInt16(std::numeric_limits<float>::quiet_NaN()) == 0,
              "a NaN quantises to silence, not to undefined behaviour");
        check(sliceAudioToInt16(std::numeric_limits<float>::infinity()) == 0,
              "an infinity quantises to silence, not a full-scale click");
        check(sliceAudioToInt16(-std::numeric_limits<float>::infinity()) == 0,
              "including a negative infinity");

        // Degenerate input must not walk off anything.
        applySliceAudioInPlace(nullptr, 4, false, 50, 50);
        float untouched[2] = {1.0f, -1.0f};
        applySliceAudioInPlace(untouched, 0, false, 0, 0);
        check(untouched[0] == 1.0f && untouched[1] == -1.0f,
              "a zero-frame block is left alone");
    }

    // ---- speaker send pacing ----
    {
        SpeakerAudioPacer pacer;
        check(pacer.estimatedFifoFrames() == 0.0, "a fresh pacer believes the FIFO is empty");
        // Empty FIFO, plenty queued: release up to the target, not everything.
        const int first = pacer.packetsToSend(100);
        check(first > 0, "an empty FIFO releases packets");
        // The target IS the burst bound -- see kTargetFifoFrames. A separate cap
        // was removed because it could not bind, and the assertion that it did
        // was removed with it.
        check(first == 16, "an empty FIFO fills exactly to the sixteen-packet target");

        // THE CHECK THE BENCH HAD TO FIND FOR US. The target must cover more than
        // one drain tick's worth of audio, or the stream delivers less than real
        // time and the radio plays a gap every few milliseconds. A 5 ms tick
        // removes 5 * 48 = 240 frames; whole-packet truncation then releases
        // floor(240/64) = 3 packets, 192 frames, so a target of only 240 frames
        // can never catch up. This is stated as frames-per-tick rather than as a
        // packet count so it stays true if the packet geometry changes.
        {
            constexpr double kDrainTickSeconds = 0.005;   // P2Client::kSpeakerDrainMs
            constexpr double kFramesPerTick = kDrainTickSeconds * kSpeakerSampleRateHz;
            check(SpeakerAudioPacer::kTargetFifoFrames > kFramesPerTick,
                  "the target holds more than one drain tick drains");
            // And by enough that truncation plus a late tick cannot empty it: two
            // ticks is the margin, which four packets did not have and sixteen does.
            check(SpeakerAudioPacer::kTargetFifoFrames >= 3.0 * kFramesPerTick,
                  "the target has margin for a late tick, not just the nominal one");
            // Releasing a full target must be worth at least one tick of real time,
            // or no burst can ever restore the deficit a late tick created.
            SpeakerAudioPacer fresh;
            const double releasedFrames =
                fresh.packetsToSend(1000) * static_cast<double>(kSpeakerFramesPerPacket);
            check(releasedFrames > kFramesPerTick,
                  "one burst delivers more audio than one tick consumes");

            // AND AT LEAST ONE SOURCE BLOCK. Audio arrives in blocks, not
            // samples: at the default 48 ksps DDC, WdspChannel's output block is
            // 1024 * 24000/48000 = 512 frames of 24 kHz audio, which is 1024
            // frames at the stream's rate. A target below one block cannot release
            // a block before the next one arrives, so the queue grows to its cap
            // and drops -- audible as garbled speech, and worst at the DEFAULT
            // rate. Spelled out rather than hardcoded so the arithmetic is
            // checkable against the DSP config.
            constexpr double kWdspInputBlock = 1024.0;
            constexpr double kDefaultDdcRate = 48000.0;
            constexpr double kAudioRate = 24000.0;
            constexpr double kSourceBlockFramesAtStreamRate =
                (kWdspInputBlock * kAudioRate / kDefaultDdcRate)
                * (static_cast<double>(kSpeakerSampleRateHz) / kAudioRate);
            check(SpeakerAudioPacer::kTargetFifoFrames >= kSourceBlockFramesAtStreamRate,
                  "the target holds at least one whole source block");
        }

        for (int i = 0; i < first; ++i) pacer.onPacketSent();
        check(pacer.estimatedFifoFrames() == first * kSpeakerFramesPerPacket,
              "each sent packet credits its own frames");
        // At target with no time passed, nothing more may go.
        check(pacer.packetsToSend(100) == 0, "a full FIFO releases nothing");

        // One packet's worth of time drains one packet's worth of frames.
        pacer.advance(static_cast<double>(kSpeakerFramesPerPacket) / kSpeakerSampleRateHz);
        check(pacer.packetsToSend(100) == 1, "one packet interval makes room for one packet");

        // Nothing queued, nothing sent, however much room there is.
        check(pacer.packetsToSend(0) == 0, "an empty queue sends nothing");

        // A long stall cannot bank negative depth and then over-release.
        pacer.advance(10.0);
        check(pacer.estimatedFifoFrames() == 0.0, "the estimate floors at empty");
        check(pacer.packetsToSend(1000) == 16,
              "recovery from a stall still respects the target, not a debt");

        // Backwards or zero elapsed time is inert, not corrupting.
        SpeakerAudioPacer p2;
        p2.onPacketSent();
        const double before = p2.estimatedFifoFrames();
        p2.advance(0.0);
        p2.advance(-1.0);
        check(p2.estimatedFifoFrames() == before, "zero or negative elapsed time changes nothing");

        p2.reset();
        check(p2.estimatedFifoFrames() == 0.0, "reset clears the estimate for the next session");

        // ---- catch-up: a backlog on OUR side must be recoverable ----
        //
        // THE G2 BENCH FOUND THIS. With only the target governing releases the
        // queue settled at 26-45 packets of 64 and stayed there: in steady state
        // the pacer releases exactly what time consumes, so a backlog picked up
        // during the connect transient is never recovered. Nominal rate is
        // break-even by definition.
        SpeakerAudioPacer p3;
        constexpr int kCap = 64;
        check(!p3.catchingUp(), "a fresh pacer is not catching up");

        // At target, and idle: nothing goes, which is the behaviour that stranded
        // the backlog.
        for (int i = 0; i < 16; ++i) p3.onPacketSent();
        check(p3.packetsToSend(40) == 0, "at target with no drain time, nothing is released");

        // ONE SOURCE BLOCK IS NOT A BACKLOG, and this is the boundary that matters
        // most here. Audio arrives one whole block at a time -- sixteen packets at
        // the default DDC rate, by the same arithmetic the target invariant above
        // spells out -- so a threshold at or below one block latches on every
        // ordinary block. Catch-up would be the steady state, the credit target
        // would govern no release at all, and a release bounded only by the queue
        // would run at four times nominal for as long as any backlog lasted.
        // Derived here rather than hardcoded, so a change of block size or packet
        // geometry moves the check with it.
        constexpr double kWdspInputBlock = 1024.0;
        constexpr double kDefaultDdcRate = 48000.0;
        constexpr double kAudioRate = 24000.0;
        constexpr int kPacketsPerSourceBlock = static_cast<int>(
            ((kWdspInputBlock * kAudioRate / kDefaultDdcRate)
             * (static_cast<double>(kSpeakerSampleRateHz) / kAudioRate))
            / kSpeakerFramesPerPacket);
        p3.setBacklog(kPacketsPerSourceBlock, kCap);
        check(!p3.catchingUp(),
              "one whole source block queued is a block arriving, not a backlog");
        p3.setBacklog(2 * kPacketsPerSourceBlock - 1, kCap);
        check(!p3.catchingUp(),
              "and anything short of a second block is still not one");

        // Two blocks is audio that arrived while we were not draining.
        p3.setBacklog(2 * kPacketsPerSourceBlock, kCap);
        check(p3.catchingUp(), "two whole source blocks queued enters catch-up");
        check(p3.packetsToSend(40) > 0,
              "catch-up releases despite the target being met -- the whole point");

        // Hysteresis: it does not drop out at the same level it entered.
        p3.setBacklog(2 * kPacketsPerSourceBlock - 1, kCap);
        check(p3.catchingUp(), "catch-up persists through the band, so it cannot flap");
        p3.setBacklog(kPacketsPerSourceBlock + 1, kCap);
        check(p3.catchingUp(), "and still persists one packet above the exit");
        p3.setBacklog(kPacketsPerSourceBlock, kCap);
        check(!p3.catchingUp(),
              "down to the block in flight, the backlog is gone and catch-up ends");
        p3.setBacklog(1, kCap);
        check(!p3.catchingUp(), "a drained queue leaves catch-up");

        // Bounded. A recovery that released without limit would overrun the very
        // FIFO this class protects.
        SpeakerAudioPacer p4;
        p4.setBacklog(kCap, kCap);
        check(p4.catchingUp() && p4.packetsToSend(10000) <= 16,
              "catch-up is bounded, not an unlimited burst");

        // A zero or negative capacity must not decide anything.
        SpeakerAudioPacer p5;
        p5.setBacklog(100, 0);
        check(!p5.catchingUp(), "an unknown capacity cannot trigger catch-up");

        // reset() clears it, or a reconnect would start mid-recovery.
        SpeakerAudioPacer p6;
        p6.setBacklog(kCap, kCap);
        p6.reset();
        check(!p6.catchingUp(), "reset clears catch-up for the next session");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_p2_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
