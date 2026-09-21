// HL2 wideband bandscope headroom — the pure decisions in Hl2BandscopeHeadroom.h.
//
// No sockets, no Qt, no hardware. Everything here is reachable by arithmetic,
// which is the whole reason the decisions live in a header rather than inline
// in Hl2Backend: on a real radio these branches need an out-of-slice signal
// strong enough to overload a converter, and a test suite cannot arrange one.
//
// THE TWO ANCHORS THAT ARE NOT OURS TO CHOOSE, and they are what make this
// test worth having:
//
//   * THE GATEWARE'S OWN THRESHOLDS. ad9866.v fires rxclip at |code| == 2048
//     and rxgoodlvl at |code| >= 1536. Our dB scale must put those at 0.00 and
//     2.50 dB of headroom or it does not agree with the converter, and every
//     statement this header makes about "below the clip point" is then false.
//
//   * THE STUDY'S PUBLISHED BIAS TABLE. wideband-bandscope-headroom-sensor.md
//     4.2 tabulates the Gaussian expected-maximum bias at six duty cycles,
//     to two decimals, computed independently of this code. gatedPeakBiasDb
//     must reproduce it. A table written down in a document and a function
//     written later are exactly the pair that silently disagrees.

#include "core/backends/hl2/Hl2BandscopeHeadroom.h"

#include <cmath>
#include <cstdio>

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

// A block whose peak is `peakAbs` and which railed `clipped` times. `samples`
// defaults to a whole gated block so the fixtures read like real ones.
static Ep4Stats block(int peakAbs, int clipped = 0, int samples = kEp4BlockSamples)
{
    Ep4Stats s;
    s.samples = samples;
    s.peakAbs = peakAbs;
    s.clippedSamples = clipped;
    // sumSquares is not read by anything under test here; leave it at zero
    // rather than inventing a distribution the fixture cannot justify.
    return s;
}

int main()
{
    // ---- 1 · the scale agrees with the converter's own thresholds ----
    //
    // The cheapest possible check that our dB scale and ad9866.v's agree, and
    // the one that would catch a kFullScale copy-paste: MetisProtocol.h's
    // kFullScale is the EP6 24-bit DDC scale and does NOT apply to these codes.
    {
        const auto atClip = bandscopeHeadroom(block(kEp4FullScale), 0);
        check(approx(atClip.headroomDb, 0.0),
              "a peak at the clip code reads 0.00 dB of headroom");

        const auto atKnee = bandscopeHeadroom(block(1536), 0);
        check(approx(atKnee.headroomDb, 2.4987747, 1e-6),
              "rxgoodlvl's knee (|code| 1536) reads 2.50 dB of headroom");
        check(atKnee.state == BandscopeHeadroom::NearRail,
              "the knee itself classifies as NearRail, not Measured");
        check(approx(kGoodLevelKneeHeadroomDb, 2.4987747, 1e-6),
              "the published knee constant is the same arithmetic");

        // Half the clip code is exactly 6.02 dB down. If this read ~66 dB more
        // it would mean the 24-bit EP6 scale had been used, which is the
        // copy-paste that would be invisible on any band not at the rail.
        const auto half = bandscopeHeadroom(block(kEp4FullScale / 2), 0);
        check(approx(half.headroomDb, 6.0205999, 1e-6),
              "half the clip code is 6.02 dB of headroom, not ~72");
        check(half.state == BandscopeHeadroom::Measured,
              "6 dB down is a Measured reading");
    }

    // ---- 2 · absent is not zero, and that is the whole contract ----
    {
        check(bandscopeHeadroom(Ep4Stats{}, 0).state == BandscopeHeadroom::Absent,
              "a block with no samples is Absent");
        check(bandscopeHeadroom(block(100), -1).state == BandscopeHeadroom::Absent,
              "a negative age -- never observed -- is Absent");
        check(bandscopeHeadroom(block(100), kHeadroomMaxAgeMs).state
                  != BandscopeHeadroom::Absent,
              "a block exactly at the age limit still counts");
        check(bandscopeHeadroom(block(100), kHeadroomMaxAgeMs + 1).state
                  == BandscopeHeadroom::Absent,
              "one millisecond past the limit is Absent");
        // The distinction that matters: an expired reading must not be usable
        // as though it said "no room", nor as though it said "plenty".
        const auto stale = bandscopeHeadroom(block(4), kHeadroomMaxAgeMs + 1);
        check(!stale.isMeasurement(), "an expired observation is not a measurement");
        check(!headroomLicensesStepDb(stale, 1.0, 0.0, 0.0),
              "an expired observation licenses nothing, however quiet it was");
    }

    // ---- 3 · a railed block has no magnitude, and says so ----
    {
        const auto railed = bandscopeHeadroom(block(kEp4FullScale, 7), 0);
        check(railed.state == BandscopeHeadroom::AtRail, "a clipped block is AtRail");
        check(railed.railed(), "AtRail reports railed()");
        check(!railed.isMeasurement(),
              "AtRail is not a measurement -- a pinned code carries no magnitude");
        check(approx(railed.headroomDb, 0.0), "AtRail reports zero headroom");
        check(railed.clippedSamples == 7, "the rail count is carried through");
        // The count is ad9866.v's asymmetric predicate, so a block can rail on
        // -2048 while its peakAbs is below the positive clip code. The
        // classification must follow the COUNT, not the peak.
        const auto negRail = bandscopeHeadroom(block(2047, 1), 0);
        check(negRail.state == BandscopeHeadroom::AtRail,
              "a negative-rail clip is AtRail even though peakAbs is 2047");
    }

    // ---- 4 · the sampling bias reproduces the study's own table ----
    //
    // wideband-bandscope-headroom-sensor.md 4.2, computed there from
    // E[max] ~ sigma*sqrt(2 ln N) against the converter's 76.8 MSPS. Two
    // decimals is what the document quotes, so two decimals is what is checked.
    {
        struct Row { double perSecond; double biasDb; const char* what; };
        const Row rows[] = {
            {   194970.0, 1.73, "continuous bandscope is 1.73 dB down" },
            {    45875.0, 2.28, "22 blocks/s is 2.28 dB down" },
            {    20480.0, 2.62, "10 blocks/s is 2.62 dB down" },
            {     4096.0, 3.39, "2 blocks/s is 3.39 dB down" },
            {     2048.0, 3.77, "1 block/s is 3.77 dB down" },
        };
        for (const Row& r : rows) {
            check(approx(std::round(gatedPeakBiasDb(r.perSecond) * 100.0) / 100.0,
                         r.biasDb, 1e-9), r.what);
        }
        check(approx(gatedPeakBiasDb(kAdcSampleRateHz), 0.0),
              "observing every sample has no bias, by definition");

        // The gate as MetisClient actually runs it: one 2048-sample block a
        // second. This is the number a consumer must budget as margin.
        check(approx(std::round(gatedPeakBiasDbForPeriod(1000) * 100.0) / 100.0,
                     3.77, 1e-9),
              "the shipped gate period carries a 3.77 dB bias bound");
        // Bounded and slowly varying: two orders of magnitude of duty cycle
        // move it about 2 dB. That property is why the duty cycle is nearly
        // free, and it is worth pinning so a future gate change is not feared.
        const double spread = gatedPeakBiasDb(2048.0) - gatedPeakBiasDb(204800.0);
        check(spread > 1.5 && spread < 2.5,
              "a hundred-fold duty-cycle change moves the bias about 2 dB");

        // Degenerate inputs return 0 rather than a NaN or a negative margin: a
        // caller adding this to a requirement must never be handed a licence
        // it had not earned.
        check(approx(gatedPeakBiasDb(0.0), 0.0), "zero samples yields no margin");
        check(approx(gatedPeakBiasDb(1e9), 0.0),
              "observing more than full rate yields no margin");
        check(approx(gatedPeakBiasDbForPeriod(0), 0.0), "a zero period yields no margin");
    }

    // ---- 5 · what the reading licenses, which is the load-bearing decision --
    //
    // The rule is headroom >= step + bias + margin. Each term is checked by
    // moving it alone, because a test that only moved the headroom would pass
    // against a function that had dropped two of the three.
    {
        const double bias = gatedPeakBiasDbForPeriod(1000);   // 3.77
        // 12 dB of room. A 6 dB step needs 6 + 3.77 + 2 = 11.77. It fits.
        const auto roomy = bandscopeHeadroom(block(515), 0);  // ~11.99 dB down
        check(roomy.state == BandscopeHeadroom::Measured, "the fixture is a measurement");
        check(roomy.headroomDb > 11.9 && roomy.headroomDb < 12.1,
              "the fixture really is about 12 dB down");
        check(headroomLicensesStepDb(roomy, 6.0, 2.0, bias),
              "12 dB of room licenses a 6 dB step with the bias and 2 dB budgeted");

        // Remove the room, keep everything else: refused.
        const auto tight = bandscopeHeadroom(block(1024), 0);  // 6.02 dB down
        check(!headroomLicensesStepDb(tight, 6.0, 2.0, bias),
              "6 dB of room does not license a 6 dB step once the bias is budgeted");
        // ... and it does not license it even with no margin at all, because
        // the bias alone already exceeds what is left over.
        check(!headroomLicensesStepDb(tight, 6.0, 0.0, bias),
              "the bias bound alone is enough to refuse a 6 dB step into 6 dB");
        // With the bias pretended away it would pass -- which is exactly the
        // mistake the bias term exists to prevent, pinned here so that
        // deleting the term fails a test rather than passing one.
        check(headroomLicensesStepDb(tight, 6.0, 0.0, 0.0),
              "without the bias term the same reading would have licensed the step");

        // A smaller step fits where a larger one does not: the step term is
        // real and is not a constant.
        check(headroomLicensesStepDb(tight, 1.0, 0.0, bias),
              "the same 6 dB of room does license a 1 dB step");

        // The margin term is real too.
        check(!headroomLicensesStepDb(roomy, 6.0, 3.0, bias),
              "raising the caller's margin alone can refuse a step that fitted");

        // Refusals that are not about arithmetic.
        check(!headroomLicensesStepDb(roomy, 0.0, 0.0, 0.0),
              "a zero step is not a step in the loud direction");
        check(!headroomLicensesStepDb(roomy, -3.0, 0.0, 0.0),
              "a negative step is refused rather than trivially licensed");
        const auto railed = bandscopeHeadroom(block(kEp4FullScale, 3), 0);
        check(!headroomLicensesStepDb(railed, 1.0, 0.0, 0.0),
              "a railed block licenses nothing");
        // NearRail is a measurement and goes through the arithmetic, failing
        // it for any useful step rather than being special-cased out.
        const auto knee = bandscopeHeadroom(block(1536), 0);
        check(knee.isMeasurement(), "NearRail is still a measurement");
        check(!headroomLicensesStepDb(knee, 1.0, 0.0, bias),
              "NearRail fails the arithmetic for a 1 dB step");
    }

    // ---- 6 · the pairing with the continuous clip flag ----
    //
    // The states that matter are the two disagreements, and one of them is a
    // trap: a clean block while the flag says the converter railed is the
    // gate's blind interval, NOT evidence against the flag.
    {
        const auto clean  = bandscopeHeadroom(block(100), 0);      // ~26 dB down
        const auto knee   = bandscopeHeadroom(block(1536), 0);
        const auto railed = bandscopeHeadroom(block(kEp4FullScale, 5), 0);
        const auto absent = bandscopeHeadroom(Ep4Stats{}, 0);

        check(bandscopeClipAgreement(absent, true, false)
                  == BandscopeClipAgreement::Unknown,
              "no block means Unknown, whatever the flag says");
        check(bandscopeClipAgreement(clean, false, false)
                  == BandscopeClipAgreement::Unknown,
              "a flag never seen means Unknown, not clear");

        check(bandscopeClipAgreement(clean, true, false)
                  == BandscopeClipAgreement::Clear,
              "flag clear and a quiet block is Clear");
        check(bandscopeClipAgreement(knee, true, false)
                  == BandscopeClipAgreement::ApproachingRail,
              "flag clear but the block at the knee is the early warning the "
              "boolean cannot give");
        check(bandscopeClipAgreement(railed, true, true)
                  == BandscopeClipAgreement::Agreed,
              "both sides railing is Agreed");

        // THE TRAP.
        check(bandscopeClipAgreement(clean, true, true)
                  == BandscopeClipAgreement::ClippedBetweenBlocks,
              "a clean block while the flag says clipped is the gate's blind "
              "interval, not a contradiction");
        // And it must not be mistakable for the quiet case: a consumer keying
        // off Clear would otherwise release gain into a clipping converter.
        check(bandscopeClipAgreement(clean, true, true)
                  != BandscopeClipAgreement::Clear,
              "the blind-interval case is never reported as Clear");

        // The other direction: the block railed and the latched flag had
        // already been cleared. Resolve toward the rail -- the safe direction,
        // and the one with direct evidence behind it.
        check(bandscopeClipAgreement(railed, true, false)
                  == BandscopeClipAgreement::Agreed,
              "a railed block with a cleared flag still reports the rail");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_bandscope_headroom_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
