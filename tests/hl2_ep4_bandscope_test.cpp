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
        // ALTERNATING and not a constant +1024. This case asserts that a
        // constant-MAGNITUDE record has zero crest, and since #5802 the RMS is
        // taken about the mean — so a record that never changes sign is not a
        // constant-magnitude signal at all, it is a DC pedestal with no AC
        // content, and its RMS is the floor. The fixture now says what the
        // assertions below have always claimed it said.
        const auto half = ep4Stats(makeEp4(0, {1024, -1024}));
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

        // OUR dB SCALE AND THE CONVERTER'S, CHECKED AGAINST EACH OTHER at the
        // two magnitudes the gateware itself names. ad9866.v derives both of
        // its level flags from the same rx_data register these codes come from:
        //
        //     rxclip    = |code| at 2048 (rxclipp +2047 / rxclipn -2048)
        //     rxgoodlvl = |code| at 1536
        //
        // so a bandscope level and the ADC-overload bit in the EP6 telemetry
        // are commensurable BY CONSTRUCTION — but only if this side normalises
        // by the converter's full scale. This is the cheapest possible check
        // that it does.
        const auto goodLvl = ep4Stats(makeEp4(0, {1536, -1536}));
        check(goodLvl.has_value(), "a good-level packet parses");
        check(goodLvl->peakAbs == 1536, "rxgoodlvl's threshold survives the decode");
        check(approx(goodLvl->peakDbfs(), -2.4988, 1e-4),
              "the gateware's rxgoodlvl (|code| 1536) is -2.50 dBFS on our scale");
        check(goodLvl->clippedSamples == 0, "rxgoodlvl is not rxclip");

        // AND THE COPY-PASTE THIS EXISTS TO CATCH. kFullScale is the EP6
        // 24-bit DDC scale; applying it to a 12-bit pre-DDC code reads every
        // bandscope block as ~66 dB quieter than it is, and the error is
        // invisible on any band that is not at the rail.
        const double wrongScale =
            20.0 * std::log10(2047.0 / static_cast<double>(kFullScale));
        check(wrongScale < -70.0,
              "normalising by the EP6 24-bit scale would read full scale as < -70 dBFS");
        check(!approx(square.peakDbfs(), wrongScale, 1.0),
              "...which is not what full scale reads here");

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
            // `sum` alongside `sumSquares`, because rmsDbfs() removes the mean
            // and a `whole` that forgot the signed sum would claim a mean of
            // zero for a record that has one — making the comparison below
            // pass for the wrong reason.
            whole.sum += s->sum;
            whole.clippedSamples += s->clippedSamples;
            if (s->peakAbs > whole.peakAbs)
                whole.peakAbs = s->peakAbs;
        }
        check(merged.samples == whole.samples, "merge sums the sample count");
        check(merged.peakAbs == whole.peakAbs, "merge takes the max peak, not the sum");
        check(approx(merged.sumSquares, whole.sumSquares), "merge sums the energy");
        check(approx(merged.sum, whole.sum), "merge sums the signed codes too");
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

    // ---- 10 · rmsDbfs() measures about the MEAN, so converter DC cannot
    //            masquerade as signal (#5802) ----
    //
    // The defect this replaces computed sqrt(sumSquares / samples), which for
    // a record with mean m and deviation s is sqrt(m^2 + s^2) and not s. It is
    // not a rounding-grade error: a terminated antenna port read adcCrestDb =
    // 3.71 dB and was diagnosed as a near-sinusoidal carrier, when a DC
    // pedestal fits that number exactly as well. Crest is the one surface that
    // separates broadband noise (11-12 dB over 2048 samples) from a discrete
    // carrier (~3 dB), and an inflated RMS deflates it toward the carrier end
    // whatever the RF is doing.
    {
        // A full-period square wave: mean exactly zero, deviation exactly its
        // amplitude, both independent of how the 512 samples divide between
        // the phases. Nothing here is a production constant — kAc and kDc are
        // fixture amplitudes, and every expectation below is derived from the
        // radio's own kEp4FullScale rather than from a retyped number.
        constexpr int kAc = 400;    // codes of AC excursion
        constexpr int kDc = 300;    // codes of pedestal laid underneath it

        const auto ac   = ep4Stats(makeEp4(0, {kAc, -kAc}));
        const auto acDc = ep4Stats(makeEp4(0, {kAc + kDc, -kAc + kDc}));
        const auto pure = ep4Stats(makeEp4(0, {kDc}));       // pedestal, no AC
        // Guarded HERE, where the dereferencing starts, and for all of them at
        // once. A guard further down protects nothing: the first unguarded
        // dereference is the one that crashes, and a crash loses every check
        // that would have run after it -- a worse diagnostic than the failure
        // it replaces.
        check(ac.has_value() && acDc.has_value() && pure.has_value(),
              "the DC fixtures parse");
        if (!ac.has_value() || !acDc.has_value() || !pure.has_value()) {
            return 1;
        }

        // THE PROPERTY. Same AC excursion, one of them sitting on a pedestal:
        // the RMS is a statement about the excursion and must not move.
        check(approx(ac->rmsDbfs(), acDc->rmsDbfs(), 1e-9),
              "a DC pedestal does not change the reported RMS");
        // And the pedestal alone has nothing to report. -inf is the true
        // answer; the floor is how this type says so.
        check(approx(pure->rmsDbfs(), kEp4FloorDbfs),
              "a record that is pure DC has no AC content and reads the floor");

        // POSITIVE CONTROL 1 · the fixture really does carry the offset, so
        // the equality above is not passing because both blocks are the same
        // block. The peak, which stays absolute, moves by the whole pedestal.
        check(acDc->peakAbs == kAc + kDc, "the offset block peaks a pedestal higher");
        check(acDc->peakDbfs() > ac->peakDbfs() + 1.0,
              "...and that is visible in peakDbfs, which is deliberately NOT mean-referred");

        // POSITIVE CONTROL 2 · the discarded statistic, computed here from the
        // struct's own accumulators, DOES separate the two blocks — and by the
        // exact amount the variance identity predicts. This is what the old
        // rmsDbfs() returned, so it measures how far apart an implementation
        // that regressed would put the two answers: a blind assertion cannot
        // produce this number.
        auto aboutZeroDbfs = [](const Ep4Stats& s) {
            return 20.0 * std::log10(std::sqrt(s.sumSquares / static_cast<double>(s.samples))
                                     / static_cast<double>(kEp4FullScale));
        };
        const double predicted =
            20.0 * std::log10(std::hypot(static_cast<double>(kAc), static_cast<double>(kDc))
                              / static_cast<double>(kAc));
        check(predicted > 1.0, "the fixture's pedestal is large enough to be a real error");
        check(approx(aboutZeroDbfs(*acDc) - aboutZeroDbfs(*ac), predicted, 1e-9),
              "measuring about zero would report the pedestal as sqrt(m^2 + s^2)");

        // POSITIVE CONTROL 3 · and the correction is not a blanket subtraction
        // that would drag every reading down. With no mean to remove, the two
        // definitions must agree to the last bit.
        check(approx(ac->rmsDbfs(), aboutZeroDbfs(*ac), 1e-12),
              "on a zero-mean record, removing the mean changes nothing");

        // THE CONSEQUENCE THE ISSUE IS ABOUT, on a DC-dominated record: the
        // old reading collapses the crest toward zero because the RMS grows
        // with the pedestal almost as fast as the peak does.
        constexpr int kSmallAc = 100;
        constexpr int kBigDc   = 1200;
        const auto swamped = ep4Stats(makeEp4(0, {kSmallAc + kBigDc, -kSmallAc + kBigDc}));
        check(swamped.has_value(), "the swamped fixture parses");
        if (!swamped.has_value()) {
            // Not `g_failures == 0 ? 0 : 1`: the check above has just failed,
            // so that expression is only ever 1 written the long way.
            return 1;
        }
        check(swamped->peakDbfs() - aboutZeroDbfs(*swamped) < 1.0,
              "about zero, a pedestal twelve times the excursion reads as under 1 dB of crest");
        check(swamped->peakDbfs() - swamped->rmsDbfs() > 10.0,
              "about the mean, the same record is nowhere near a carrier's crest");

        // Mean removal belongs to the BLOCK, not to the packet. Each packet
        // carries its OWN pedestal, because that is the only way the two
        // answers differ: with one pedestal shared by all four, centring per
        // packet and centring over the concatenation give the SAME variance
        // and the check passes either way. Verified by mutation — with
        // ep4Stats() patched to subtract its own mean, the shared-pedestal
        // form still passed while claiming to catch exactly that.
        //
        // With distinct pedestals the block's variance is the AC variance plus
        // the variance of the four pedestals, and a per-packet subtraction
        // reports the AC variance alone.
        Ep4Stats block;
        double dcSum = 0.0;
        double dcSumSquares = 0.0;
        for (int p = 0; p < kEp4PacketsPerBlock; ++p) {
            const int dc = kDc * (p + 1);
            const auto pkt =
                ep4Stats(makeEp4(static_cast<std::uint32_t>(p), {kAc + dc, -kAc + dc}));
            check(pkt.has_value(), "the per-packet pedestal fixtures parse");
            if (!pkt.has_value()) {
                return 1;
            }
            block.merge(*pkt);
            dcSum += dc;
            dcSumSquares += static_cast<double>(dc) * static_cast<double>(dc);
        }
        const double packets = static_cast<double>(kEp4PacketsPerBlock);
        const double dcVar =
            dcSumSquares / packets - (dcSum / packets) * (dcSum / packets);
        check(dcVar > 0.0, "the four pedestals really do differ");
        const double expectedRms =
            std::sqrt(static_cast<double>(kAc) * static_cast<double>(kAc) + dcVar);
        check(block.samples == kEp4BlockSamples, "four offset packets make a block");
        check(approx(block.rmsDbfs(),
                     20.0 * std::log10(expectedRms / static_cast<double>(kEp4FullScale)),
                     1e-9),
              "the mean is removed once, over the merged block, not per packet");
    }

    // ---- 11 · crestDb() declines to exist rather than subtracting a sentinel
    //            (#5802; the floor nit on PR #5832) ----
    //
    // adcCrestDb is peak - rms, and kEp4FloorDbfs is not a level: it is the
    // marker this type uses to say "below the smallest code this converter
    // has" WITHOUT inventing one. Making the RMS AC-referred is what made the
    // pairing reachable — before it, an about-zero RMS sat at the floor only
    // when the peak did too, and the difference was a harmless zero. Now a DC
    // pedestal has a real peak and no representable deviation, and the naive
    // subtraction publishes tens of dB of "crest" for a record that has none.
    {
        constexpr int kDc = 1200;

        // A normal record: crest exists and is exactly the two rows'
        // difference, so this is a guard and not a reinterpretation.
        const auto ac = ep4Stats(makeEp4(0, {400, -400}));
        check(ac.has_value(), "the AC fixture parses");
        if (!ac.has_value()) {
            return 1;
        }
        const std::optional<double> acCrest = ac->crestDb();
        check(acCrest.has_value(), "a record with AC content has a crest");
        check(acCrest.has_value()
                  && approx(*acCrest, ac->peakDbfs() - ac->rmsDbfs(), 1e-12),
              "...and it is peak - rms, unchanged");

        // PURE PEDESTAL. The peak is real and large; there is no AC at all.
        const auto pure = ep4Stats(makeEp4(0, {kDc}));
        check(pure.has_value(), "the pedestal fixture parses");
        if (!pure.has_value()) {
            return 1;
        }
        check(approx(pure->rmsDbfs(), kEp4FloorDbfs), "the pedestal's RMS is the floor");
        check(pure->peakDbfs() > kEp4FloorDbfs + 40.0,
              "...while its peak is a real level tens of dB above the floor");
        check(!pure->crestDb().has_value(),
              "a pedestal with no AC content has no crest to report");
        // POSITIVE CONTROL: the number the naive subtraction would have
        // published, derived here rather than retyped, so the assertion above
        // is measured against the size of the error it prevents.
        check(pure->peakDbfs() - pure->rmsDbfs() > 60.0,
              "peak - floor would have fabricated over 60 dB of crest");

        // SUB-FLOOR AC. One code of wobble on a 512-sample pedestal is a
        // deviation of sqrt(511)/512 ~ 0.044 codes — under half a code, so
        // below the floor rather than at it. A ratio against quantisation
        // residue is not a measurement.
        std::vector<int> wobble(kEp4SamplesPerPacket, kDc);
        wobble[0] = kDc + 1;
        const auto lsb = ep4Stats(makeEp4(0, wobble));
        check(lsb.has_value(), "the one-LSB-wobble fixture parses");
        if (!lsb.has_value()) {
            return 1;
        }
        check(lsb->rmsDbfs() < kEp4FloorDbfs,
              "a sub-half-code deviation computes BELOW the floor, not at it");
        check(!lsb->crestDb().has_value(),
              "and it has no crest either, for the same reason");

        // An all-zero block: both terms are the floor. The naive form gives a
        // harmless zero, so this one is about the rule, not the damage.
        const auto silent = ep4Stats(makeEp4(0, {0}));
        check(silent.has_value(), "the all-zero fixture parses");
        if (!silent.has_value()) {
            return 1;
        }
        check(!silent->crestDb().has_value(), "an all-zero block reports no crest");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_bandscope_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
