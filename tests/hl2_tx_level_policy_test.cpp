// The HL2 transmit level calculations: mic gain, and the forward-power peak hold.
//
// Both fixed controls that were DEAD rather than wrong, which is why the cases
// below lean on the boundaries rather than the middle of the range:
//
//   - Hl2TxDsp::setMicGain had no production caller at all. The Phone applet's
//     MIC slider emitted `transmit set miclevel=`, which a backend with no
//     command plane drops, and nothing bridged it to the modulator. Moving the
//     slider end to end changed nothing on the air.
//   - Forward power was published as the raw instantaneous sample from a 10 Hz
//     I2C instrumentation ADC. On constant-envelope FT8 that reads PEP; on
//     speech it reads ~10 dB low, so the same transmitter measured 6 W on FT8
//     and 1 W on voice and the operator had no way to tell those apart.
//
// Hl2Backend evaluates these same functions rather than its own copy, so what
// passes here is what the radio runs (core/backends/hl2/Hl2TxLevelPolicy.h).

#include "core/backends/hl2/Hl2TxLevelPolicy.h"

#include <cmath>
#include <initializer_list>
#include <cstdio>

using AetherSDR::hl2::fwdPeakHoldStep;
using AetherSDR::hl2::micLevelFromCurve1;
using AetherSDR::hl2::micSliderToGainDb;
using AetherSDR::hl2::micSliderToLinear;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

bool near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

// The release constant Hl2Backend ships. Duplicated deliberately: if the
// backend's value changes, the convergence case below should be re-reasoned
// rather than silently tracking it.
constexpr double kRelease = 0.05;

}  // namespace

int main()
{
    // ---- Mic gain ----------------------------------------------------------

    // THE ONE THAT PROTECTS EXISTING INSTALLS. TransmitModel constructs
    // m_micLevel at 50 and no startup path restores it, so 50 must land exactly
    // on the modulator's own 1.0 default. Any other unity point would silently
    // change the transmit level of every HL2 station the first time this shipped.
    check(near(micSliderToGainDb(50), 0.0), "slider 50 is unity gain (0 dB)");
    check(near(micSliderToLinear(50), 1.0), "slider 50 is unity linear (1.0)");

    check(near(micSliderToGainDb(100), 40.0), "slider 100 is +40 dB");
    check(near(micSliderToGainDb(1), -19.6), "slider 1 is -19.6 dB");

    // THE JOIN, which is what stops someone tidying this back to symmetric.
    //
    // The mapping is asymmetric on purpose: 0.4 dB per step below 50, 0.8 dB
    // per step above it, so the travel is -20/+40 dB with unity still exactly
    // at 50. A symmetric widening would be the obvious simplification and would
    // move unity off 50, changing the transmit level of every existing install
    // — the case above says why that may not happen, and these two say where
    // the mapping would have to break to allow it. Continuous in value, with a
    // deliberate step in slope.
    check(near(micSliderToGainDb(49), -0.4), "one step below unity is -0.4 dB");
    check(near(micSliderToGainDb(51), 0.8), "one step above unity is +0.8 dB");

    // A slider at the bottom means OFF. Without the special case it would be
    // -20 dB — which, now that the ALC only reduces and has no makeup gain to
    // haul it back up with, is a real -20 dB on the air rather than something
    // indistinguishable from "50". The behaviour does not move; the reason for
    // it is now simply that the bottom of a level control means off.
    check(near(micSliderToLinear(0), 0.0), "slider 0 mutes rather than attenuating");
    check(micSliderToLinear(1) > 0.0, "slider 1 is quiet but not muted");

    // Monotonic across the travel, so no slider position is louder than one
    // above it.
    {
        bool monotonic = true;
        for (int lvl = 1; lvl < 100; ++lvl) {
            if (micSliderToLinear(lvl + 1) <= micSliderToLinear(lvl))
                monotonic = false;
        }
        check(monotonic, "gain rises monotonically from slider 1 to 100");
    }

    // Out-of-range input is clamped, not extrapolated: a CAT client or a bridge
    // verb can pass anything, and 200 must not become +120 dB on the air.
    check(near(micSliderToGainDb(200), 40.0), "over-range level clamps to +40 dB");
    check(near(micSliderToGainDb(-50), -20.0), "under-range level clamps to -20 dB");
    check(near(micSliderToLinear(-50), 0.0), "negative level mutes");

    // ---- Curve-1 migration -------------------------------------------------
    //
    // The property that matters is not the arithmetic, it is that the GAIN is
    // preserved: a position stored against curve 1 must restore to whatever
    // curve-2 position puts the same dB on the air. Asserting the dB rather
    // than the number is what makes this a migration test instead of a copy of
    // the formula — a re-typed formula would agree with itself.
    {
        const auto curve1Db = [](int level) {
            const int clamped = level < 0 ? 0 : (level > 100 ? 100 : level);
            return (static_cast<double>(clamped) - 50.0) * 0.4;
        };
        // Even offsets from unity land exactly; curve 2 has half the resolution
        // above unity, so nothing finer than 2 steps can.
        for (const int stored : {52, 60, 70, 80, 90, 100})
            check(near(micSliderToGainDb(micLevelFromCurve1(stored)),
                       curve1Db(stored)),
                  "an even curve-1 position migrates to the same gain");

        // Odd offsets round UP, by at most one curve-2 step of 0.8 dB — half a
        // curve-1 step. The direction is deliberate: rounding down rounds back
        // toward the unity the operator moved away from.
        for (const int stored : {51, 61, 75, 99}) {
            const double moved = micSliderToGainDb(micLevelFromCurve1(stored))
                               - curve1Db(stored);
            check(moved >= 0.0 && moved <= 0.401,
                  "an odd curve-1 position rounds up by at most 0.4 dB");
        }

        // The identity half. Below unity both curves are 0.4 dB per step, so
        // moving a stored position there would be inventing a setpoint.
        for (const int stored : {0, 1, 25, 49, 50})
            check(micLevelFromCurve1(stored) == stored,
                  "a curve-1 position at or below unity is unchanged");

        // Monotone and in range, because a migration that reorders positions or
        // leaves the travel is worse than one that is merely imprecise.
        bool monotone = true;
        bool inTravel = true;
        int previous = -1;
        for (int stored = 0; stored <= 100; ++stored) {
            const int migrated = micLevelFromCurve1(stored);
            monotone = monotone && migrated >= previous;
            inTravel = inTravel && migrated >= 0 && migrated <= 100;
            previous = migrated;
        }
        check(monotone, "the migration is monotone across the whole travel");
        check(inTravel, "the migration never leaves the slider's travel");

        // Applying it twice must not move a level twice — the stamp is what
        // makes it one-shot in Hl2Backend, but the arithmetic should not punish
        // a document that loses its stamp either.
        check(micLevelFromCurve1(micLevelFromCurve1(100)) == 63,
              "a second pass moves it again — the stamp, not the arithmetic, "
              "is what makes the migration one-shot");
    }

    // ---- Forward-power peak hold -------------------------------------------

    // Attack is instant, so a peak that IS sampled is never averaged away —
    // which is the whole failure being fixed.
    check(near(fwdPeakHoldStep(1.0, 6.0, /*keyed=*/true, kRelease), 6.0),
          "a higher sample is taken immediately");

    // Release is gradual, so the reading does not fall back to the average
    // between syllables.
    {
        const double held = fwdPeakHoldStep(6.0, 1.0, /*keyed=*/true, kRelease);
        check(held > 5.7 && held < 6.0, "a lower sample releases slowly, not instantly");
    }

    // THE BEHAVIOUR THE FIX EXISTS FOR: speech sampled at 10 Hz lands mostly on
    // the troughs. Feed a run of low samples with occasional peaks — the shape
    // of a real over — and the reading must converge UP toward the peak rather
    // than settling near the average of the samples.
    {
        double peak = 0.0;
        const double kPeakW = 6.0;
        const double kTroughW = 0.7;
        // 3 seconds at 10 Hz. Every fifth sample catches a syllable.
        for (int i = 0; i < 30; ++i) {
            const double sample = (i % 5 == 0) ? kPeakW : kTroughW;
            peak = fwdPeakHoldStep(peak, sample, /*keyed=*/true, kRelease);
        }
        check(peak > 5.0,
              "over a 3 s speech-shaped run the hold converges toward PEP, not the average");
        // And it is bounded by the real peak — a hold must never invent power
        // that was not measured.
        check(peak <= kPeakW, "the hold never exceeds the largest sample seen");
    }

    // Unkeyed, the reading follows the sample straight down. A hold that
    // outlived the transmission would keep re-arming MeterModel's filter and
    // leave the gauge claiming power out of a radio that has stopped.
    check(near(fwdPeakHoldStep(6.0, 0.0, /*keyed=*/false, kRelease), 0.0),
          "unkeyed, the reading drops to the instantaneous sample at once");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
