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
#include <cstdio>

using AetherSDR::hl2::fwdPeakHoldStep;
using AetherSDR::hl2::micSliderToGainDb;
using AetherSDR::hl2::micSliderToLinear;
using AetherSDR::hl2::initialDrivePercent;

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

    check(near(micSliderToGainDb(100), 20.0), "slider 100 is +20 dB");
    check(near(micSliderToGainDb(1), -19.6), "slider 1 is -19.6 dB");

    // A slider at the bottom means OFF. Without the special case it would be
    // -20 dB, which the ALC's 40 dB of makeup would haul straight back up —
    // making "0" sound much like "50".
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
    // verb can pass anything, and 200 must not become +60 dB on the air.
    check(near(micSliderToGainDb(200), 20.0), "over-range level clamps to +20 dB");
    check(near(micSliderToGainDb(-50), -20.0), "under-range level clamps to -20 dB");
    check(near(micSliderToLinear(-50), 0.0), "negative level mutes");

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

    // ---- What a session comes up at ----------------------------------------
    //
    // The regression this pins: on a fresh install the backend's
    // m_rfPowerPercent and TransmitModel::m_rfPower both construct at 100, and
    // RadioModel's connect push is value-identical to that — so setTxPower()'s
    // change-gate correctly declines to record it while applyDrive() still
    // writes it. The radio came up at drive 255 with the PA enabled and the
    // first key-up was at full power.
    //
    // Written so it FAILS on the old behaviour rather than merely passing on
    // the new one: the fresh-install case asserts 0 and explicitly asserts NOT
    // 100, which is what the code did before.
    {
        constexpr int kAbsent = -1;
        constexpr int kMax = 255;

        // Fresh install: no band entry, no baseline. The whole point.
        check(initialDrivePercent(kAbsent, kAbsent) == 0,
              "a fresh install comes up at drive 0");
        check(initialDrivePercent(kAbsent, kAbsent) != 100,
              "a fresh install does NOT come up at the model default of 100");
        // ...and 0 means the PA is off, which is the property that matters:
        // applyDrive maps percent onto 0..kTxDriveMax and
        // MetisClient::setTxDriveLevel sends ccTxDrive(level, level > 0), so a
        // zero percent is a zero byte is a clear PA-enable bit.
        check(initialDrivePercent(kAbsent, kAbsent) * kMax / 100 == 0,
              "that 0 reaches the wire as drive byte 0, so the PA bit is clear");

        // A restored station is untouched: the start band's own entry wins.
        check(initialDrivePercent(90, 1) == 90,
              "the start band's remembered drive wins over the baseline");
        check(initialDrivePercent(65, 1) == 65, "and on another band, likewise");

        // No entry for this band, but a baseline exists: the baseline applies.
        // This is the only case defaultPercent governs.
        check(initialDrivePercent(kAbsent, 1) == 1,
              "with no band entry, the operator's baseline applies");
        check(initialDrivePercent(kAbsent, 0) == 0,
              "a baseline of 0 is honoured, not treated as absent");

        // A stored 0 for the band is a real setting, not a missing one.
        check(initialDrivePercent(0, 90) == 0,
              "a band entry of 0 wins over a non-zero baseline");

        // Out of range is clamped, never wrapped.
        check(initialDrivePercent(140, kAbsent) == 100, "a high entry clamps");

        // THE COUNTERFACTUAL, asserted rather than left in a commit message.
        // Before this change a fresh install came up at the model's
        // construction default of 100, and these two lines are what that meant
        // on the wire. They are here so the regression is legible from the test
        // itself: if someone restores the old behaviour, the first check above
        // fails and these say what the consequence was.
        constexpr int kOldComeUpPercent = 100;
        check(kOldComeUpPercent * kMax / 100 == 255,
              "the old come-up value of 100% was drive byte 255 (full)");
        check((kOldComeUpPercent * kMax / 100) > 0,
              "...and a non-zero byte sets the PA-enable bit, so the PA was on");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
