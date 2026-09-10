// HL2 wideband bandscope (endpoint 0x04) — parser unit test. Pure protocol:
// no sockets, no Qt, no hardware, compiling MetisProtocol.cpp directly the way
// hl2_metis_protocol_test does.
//
// What is under test is the decode of a stream that is NOT IQ: 512 raw 12-bit
// AD9866 codes per datagram, little-endian and shifted left by four, carried on
// the same socket as EP6 behind a 20-bit sequence counter of its own.
//
// TWO OF THESE CASES EXIST BECAUSE OF A BENCH RUN, not because of the protocol
// document, and they are the ones that would fail a parser written from the
// gateware reading alone:
//
//   * a fresh stream emits 0, 1, 2 and then RESTARTS AT 0 — usopenhpsdr1.v
//     forces `ep4_seq_no`'s low two bits to zero while the capture FIFO is not
//     yet full. Measured three times in six legs, always in the first tens of
//     milliseconds. A 32-bit-style gap detector reads that `2 -> 0` as a
//     forward gap of 1,048,574 and reports a million lost packets in the first
//     second of every session;
//   * a genuine forward skip must still count, so the guard cannot simply
//     ignore every discontinuity.
//
// Sequence-number expectations are grounded in the recorded arrivals of that
// run (see tests/Hl2Ep4ArrivalsD94.h), so what is asserted here is what a real
// radio actually sent rather than what a fixture author imagined it would.

#include "core/backends/hl2/MetisProtocol.h"

#include "Hl2Ep4ArrivalsD94.h"

#include <array>
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

static bool approx(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

// A synthetic EP4 datagram: `EF FE 01 04`, the 20-bit sequence in bytes 4..7
// big-endian with byte 4 hardwired 0x00, then 512 little-endian words each
// holding a 12-bit code shifted left by four.
static std::vector<std::uint8_t> makeEp4(std::uint32_t seq,
                                         const std::vector<int>& codes)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x04;
    pkt[4] = 0x00;                                          // hardwired
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0x0F); // ep4_seq_no[19:16]
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    for (std::size_t i = 0; i < kEp4SamplesPerPacket; ++i) {
        const int code = codes.empty() ? 0 : codes[i % codes.size()];
        const unsigned w = (static_cast<unsigned>(code) << 4) & 0xFFFFu;
        pkt[8 + 2 * i]     = static_cast<std::uint8_t>(w & 0xFF);        // low first
        pkt[8 + 2 * i + 1] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
    }
    return pkt;
}

// Replay a sequence through the guard the client applies, and report what it
// classified. This is the production rule (ep4SeqStep), not a copy of it.
struct Replay {
    std::uint64_t drops = 0;
    std::uint64_t rewinds = 0;
};
static Replay replay(const std::uint32_t* seqs, std::size_t n)
{
    Replay r;
    std::uint32_t expected = 0;
    bool have = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (have) {
            const Ep4SeqStep step = ep4SeqStep(expected, seqs[i]);
            r.drops += step.drops;
            r.rewinds += step.rewind ? 1u : 0u;
        }
        expected = (seqs[i] + 1) & (kEp4SeqModulus - 1);
        have = true;
    }
    return r;
}

int main()
{
    // ---- 1 · the `<< 4` encoding, at both rails and either side of zero ----
    {
        const std::vector<int> codes = {2047, -2048, 0, 1};
        const auto pkt = makeEp4(0, codes);
        std::vector<float> out;
        const int n = ep4Samples(pkt, out);
        check(n == static_cast<int>(kEp4SamplesPerPacket), "512 samples per EP4 packet");
        check(out.size() == kEp4SamplesPerPacket, "samples are appended, count matches");
        const float fs = static_cast<float>(kEp4FullScale);
        check(approx(out[0], 2047.0f / fs), "+2047 survives the shift");
        // The negative rail is the case a logical (rather than arithmetic)
        // recovery gets wrong, and gets wrong by turning it into +2048.
        check(approx(out[1], -2048.0f / fs), "-2048 sign-extends, not wraps");
        check(approx(out[2], 0.0), "zero decodes to zero");
        check(approx(out[3], 1.0f / fs), "+1 is one LSB, not sixteen");
        // The low nibble the gateware pads with must not leak into the code.
        check(out.size() == kEp4SamplesPerPacket, "no extra sample from the pad nibble");
    }

    // ---- 2 · little-endian, low nibble first ----
    {
        auto pkt = makeEp4(0, {1234});
        std::vector<float> straight;
        ep4Samples(pkt, straight);
        for (std::size_t i = 0; i < kEp4SamplesPerPacket; ++i)
            std::swap(pkt[8 + 2 * i], pkt[8 + 2 * i + 1]);
        std::vector<float> swapped;
        ep4Samples(pkt, swapped);
        check(!approx(straight[0], swapped[0]),
              "byte order is load-bearing: swapping the payload bytes changes the sample");
    }

    // ---- 3 · the 20-bit sequence ----
    {
        auto pkt = makeEp4(0, {0});
        pkt[4] = 0x00; pkt[5] = 0x0F; pkt[6] = 0xAB; pkt[7] = 0xCD;
        const auto seq = ep4Seq(pkt);
        check(seq.has_value() && *seq == 0x0FABCDu, "byte 5 carries ep4_seq_no[19:16]");

        // The counter is [19:0] and wraps at 1,048,576. Reading it as EP6's 32
        // bits reports one enormous gap per wrap, ~46 minutes into a session.
        const std::uint32_t wrapPair[2] = {kEp4SeqModulus - 1, 0};
        const Replay w = replay(wrapPair, 2);
        check(w.drops == 0 && w.rewinds == 0,
              "0xFFFFF -> 0 is a wrap, not a gap and not a rewind");

        // A header carrying more than 20 bits cannot walk the expected-sequence
        // state outside the modulus.
        pkt[4] = 0xFF; pkt[5] = 0xFF;
        const auto masked = ep4Seq(pkt);
        check(masked.has_value() && *masked < kEp4SeqModulus,
              "a malformed header is masked into the modulus, not trusted");
    }

    // ---- 4 · rejection ----
    {
        std::vector<std::uint8_t> ep6(kUsbPacketSize, 0);
        ep6[0] = 0xEF; ep6[1] = 0xFE; ep6[2] = 0x01; ep6[3] = 0x06;
        check(!ep4Seq(ep6).has_value(), "an EP6 packet is not an EP4 packet");
        check(!ep4Stats(ep6).has_value(), "ep4Stats refuses EP6");
        std::vector<float> out;
        check(ep4Samples(ep6, out) == -1, "ep4Samples refuses EP6");
        check(out.empty(), "a refused packet appends nothing");

        std::vector<std::uint8_t> discovery(60, 0);
        discovery[0] = 0xEF; discovery[1] = 0xFE; discovery[2] = 0x02;
        check(!ep4Seq(discovery).has_value(), "a discovery reply is not an EP4 packet");

        auto truncated = makeEp4(7, {5});
        truncated.resize(kUsbPacketSize - 1);
        check(!ep4Seq(truncated).has_value(), "1031 bytes is not an EP4 packet");
        check(!ep4Stats(truncated).has_value(), "ep4Stats refuses a short datagram");

        // And the EP6 reader must not claim an EP4 datagram either: they share a
        // socket, and a parser that accepted both would decode ADC codes as IQ.
        check(!ep6Seq(makeEp4(3, {1})).has_value(), "ep6Seq refuses an EP4 packet");
    }

    // ---- 5 · Ep4Stats arithmetic, on the converter's own scale ----
    {
        const auto half = ep4Stats(makeEp4(0, {1024}));
        check(half.has_value(), "ep4Stats accepts a well-formed packet");
        check(half->samples == static_cast<int>(kEp4SamplesPerPacket), "512 samples counted");
        check(half->peakAbs == 1024, "peak is the code, not the wire word");
        // Constant magnitude: peak and RMS agree, so crest is zero.
        check(approx(half->peakDbfs(), -6.0206, 1e-4), "half scale is -6.02 dBFS");
        check(approx(half->rmsDbfs(), -6.0206, 1e-4), "constant magnitude: rms == peak");
        check(approx(half->peakDbfs() - half->rmsDbfs(), 0.0, 1e-9),
              "crest of a constant-magnitude block is 0 dB");
        check(half->clippedSamples == 0, "half scale is not a clip");

        // A full-scale square wave rails in BOTH directions, and the positive
        // rail of a 12-bit two's-complement converter is +2047, not +2048. A
        // symmetric `abs(code) >= 2048` predicate silently misses half of it.
        Ep4Stats square;
        for (int p = 0; p < kEp4PacketsPerBlock; ++p) {
            const auto s = ep4Stats(makeEp4(static_cast<std::uint32_t>(p), {2047, -2048}));
            check(s.has_value(), "full-scale packet parses");
            square.merge(*s);
        }
        check(square.samples == kEp4BlockSamples, "four packets make a 2048-sample block");
        check(approx(square.peakDbfs(), 0.0, 1e-9), "full scale reads 0.00 dBFS");
        check(square.clippedSamples == kEp4BlockSamples,
              "both rails count as clipped, +2047 included");

        // Silence has no representable level. It must not read as full scale,
        // and it must not read as an infinity nothing downstream can render.
        const auto quiet = ep4Stats(makeEp4(0, {0}));
        check(approx(quiet->peakDbfs(), kEp4FloorDbfs), "an all-zero block reads the floor");
        check(std::isfinite(quiet->rmsDbfs()), "the floor is finite");
    }

    // ---- 6 · merge() over four packets == the statistic of the whole ----
    {
        const std::vector<int> a = {100, -200, 300};
        const std::vector<int> b = {-1500, 7, 1499};
        Ep4Stats merged;
        Ep4Stats whole;
        for (int p = 0; p < kEp4PacketsPerBlock; ++p) {
            const auto s = ep4Stats(makeEp4(static_cast<std::uint32_t>(p), (p % 2) ? b : a));
            merged.merge(*s);
            // The same numbers accumulated as one long record.
            whole.samples += s->samples;
            whole.sumSquares += s->sumSquares;
            whole.clippedSamples += s->clippedSamples;
            if (s->peakAbs > whole.peakAbs)
                whole.peakAbs = s->peakAbs;
        }
        check(merged.samples == whole.samples, "merge sums the sample count");
        check(merged.peakAbs == whole.peakAbs, "merge takes the max peak, not the sum");
        check(approx(merged.sumSquares, whole.sumSquares), "merge sums the energy");
        check(approx(merged.rmsDbfs(), whole.rmsDbfs()),
              "a block's rms is the rms of the concatenation");
    }

    // ---- 7 · THE MEASURED REWIND. 0,1,2 -> 0 is a reset, not a million drops ----
    {
        const std::uint32_t seqs[7] = {0, 1, 2, 0, 1, 2, 3};
        const Replay r = replay(seqs, 7);
        check(r.rewinds == 1, "the start-of-stream rewind is counted as a rewind");
        check(r.drops == 0, "the start-of-stream rewind is NOT counted as a drop");

        // The number the unguarded reading produces, stated so a regression is
        // recognisable when it appears in a health dialog.
        const std::uint32_t naive = (0u - 3u) & (kEp4SeqModulus - 1);
        check(naive == 1048573u, "unguarded, that step reads as 1,048,573 lost packets");
    }

    // ---- 8 · and a genuine forward skip still counts ----
    {
        const std::uint32_t seqs[3] = {0, 1, 3};
        const Replay r = replay(seqs, 3);
        check(r.drops == 1, "a forward skip of one is one drop");
        check(r.rewinds == 0, "a forward skip is not a rewind");

        // Loss and reset are adjacent in the counter's arithmetic, so the
        // boundary is asserted rather than assumed.
        check(ep4SeqStep(0, kEp4SeqForwardGapMax - 1).drops == kEp4SeqForwardGapMax - 1,
              "the largest forward gap still reads as loss");
        check(ep4SeqStep(0, kEp4SeqForwardGapMax).rewind,
              "half a modulus ahead is read as a rewind, exactly as EP6 reads it");
    }

    // ---- 9 · the recorded stream, replayed ----
    //
    // 3684 real arrivals from a v74.2 board. One rewind, in the first three
    // packets, and nothing else for ten seconds — which is the claim the guard
    // has to satisfy on real traffic and not only on a hand-built fixture.
    {
        const Replay r = replay(kD94Ep4Seq48k, kD94Ep4Seq48kCount);
        check(kD94Ep4Seq48kCount == 3684, "the recorded leg holds 3684 EP4 arrivals");
        check(r.rewinds == 1, "the recorded leg contains exactly one rewind");
        check(r.drops == 0, "the recorded leg lost no packets");
        check(kD94Ep4Seq48k[0] == 0 && kD94Ep4Seq48k[1] == 1 && kD94Ep4Seq48k[2] == 2
                  && kD94Ep4Seq48k[3] == 0,
              "the recorded rewind is the 0,1,2 -> 0 the gateware forces");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_bandscope_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
