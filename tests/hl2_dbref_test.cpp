// Every LNA gain change shifts the absolute signal reference by exactly the same
// amount. If the panadapter displays dBm and the gain moves, the whole trace
// jumps and the waterfall paints a horizontal band that reads as a real on-air
// event. Hl2DbReference exists so the gain value and its compensating offset
// live in one object and cannot drift apart.
//
// The same gain change also moves the AGC ceiling's footing -- the half the
// operator hears rather than sees -- so this pins that too.
//
// This asserts the property that matters: a signal of CONSTANT strength reports
// a CONSTANT dBm across a gain change.

#include "core/backends/hl2/Hl2DbReference.h"

#include <cmath>
#include <cstdio>

using AetherSDR::hl2::Hl2DbReference;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) < tol; }

int main()
{
    Hl2DbReference ref;

    // Uncalibrated by default: reports dBFS unchanged, exactly as this backend
    // did before the type existed. No silent recalibration.
    check(!ref.isCalibrated(), "defaults to uncalibrated");
    check(near(ref.toDbm(-73.0), -73.0),
          "uncalibrated pass-through at the default gain is identity");
    check(near(ref.offsetDb(), 0.0), "uncalibrated offset at the default gain is zero");

    // The regression this guards: subtracting the gain ABSOLUTELY rather than
    // relative to the reference moved the whole displayed floor by 20 dB.
    ref.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb);
    check(near(ref.toDbm(-120.0), -120.0),
          "default gain leaves the displayed floor exactly where it was");

    // A fixed antenna signal. Raising the LNA by 20 dB raises the digitised
    // level by 20 dB -- and must NOT change the reported strength.
    ref.setFullScaleDbm(-60.0);
    ref.setLnaGainDb(0.0);
    const double reported = ref.toDbm(-13.0);          // -13 dBFS at 0 dB gain

    ref.setLnaGainDb(20.0);
    check(near(ref.toDbm(-13.0 + 20.0), reported),
          "a 20 dB gain increase does not move the reported dBm");

    ref.setLnaGainDb(-12.0);                            // the AD9866 floor, real
    check(near(ref.toDbm(-13.0 - 12.0), reported),
          "a 12 dB gain cut does not move the reported dBm");

    // COMMANDED 48, not 48 dB of gain: the AD9866 folds code & 0x1F above code
    // 31, so this applies 16 dB on real hardware (upstream #177). What is
    // asserted here is the ARITHMETIC — given the gain it is told about, the
    // reference removes it exactly. The gap between the commanded code and the
    // applied gain is a seam problem, named in the class header.
    ref.setLnaGainDb(48.0);
    check(near(ref.toDbm(-13.0 + 48.0), reported),
          "the reference removes whatever gain it is told about, exactly");

    // The spectrum path applies offsetDb() per frame rather than toDbm() per
    // bin; the two must agree or the trace and the S-meter would disagree.
    ref.setLnaGainDb(20.0);
    check(near(-13.0 + ref.offsetDb(), ref.toDbm(-13.0)),
          "offsetDb() and toDbm() agree (spectrum vs S-meter)");

    // ---------------------------------------------------------------
    // THE AGC CEILING, WHICH IS THE HALF THE OPERATOR HEARS
    //
    // Same invariant, different sense. WDSP's maximum gain is a setpoint about
    // the signal AT THE ANTENNA applied to a signal that has already been
    // through the LNA, so an RF gain change that leaves the ceiling alone
    // changes how far down into the noise the AGC chases. THE EXPECTED DELTA
    // IS ZERO HERE TOO -- not the step size.
    // ---------------------------------------------------------------
    Hl2DbReference agc;
    constexpr int kDefaultThresholdUnits = 65;   // Hl2Backend::Receiver's default

    // No change for an operator who never touches RF gain: at the reference
    // gain this is the plain 0.6-per-unit map, the same number the backend
    // derived before this term existed.
    check(near(agc.agcCeilingDb(kDefaultThresholdUnits), 39.0),
          "at the reference gain the default AGC-T is still 39 dB");
    check(near(agc.agcCeilingDb(0), 0.0), "AGC-T 0 is still 0 dB");
    check(near(agc.agcCeilingDb(100), 60.0), "AGC-T 100 is still 60 dB");

    // A gain change moves the ceiling by exactly minus the gain change, so a
    // constant antenna signal keeps a constant heard level.
    const double ceilingAtReference = agc.agcCeilingDb(kDefaultThresholdUnits);

    agc.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb + 6.0);
    check(near(agc.agcCeilingDb(kDefaultThresholdUnits), ceilingAtReference - 6.0),
          "a 6 dB LNA rise lowers the AGC ceiling by 6 dB");
    check(near(agc.agcCeilingDb(kDefaultThresholdUnits) + 6.0, ceilingAtReference),
          "the antenna-referred ceiling does not move on a 6 dB rise");

    agc.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb - 6.0);
    check(near(agc.agcCeilingDb(kDefaultThresholdUnits), ceilingAtReference + 6.0),
          "a 6 dB LNA cut raises the AGC ceiling by 6 dB");

    // Item 14's regulator steps the RxPGA 3-6 dB several times a day. Every
    // step in a run must leave the antenna-referred ceiling exactly where it
    // started, or the regulator walks the operator's AGC over an afternoon.
    for (const double step : {3.0, -6.0, 6.0, -3.0, 4.0}) {
        agc.setLnaGainDb(agc.lnaGainDb() + step);
        check(near(agc.agcCeilingDb(kDefaultThresholdUnits) + agc.lnaGainDb(),
                   ceilingAtReference + Hl2DbReference::kDefaultLnaGainDb),
              "a regulator step leaves the antenna-referred ceiling unmoved");
    }

    // The commanded limits, where referring saturates. A negative ceiling would
    // be the AGC attenuating a signal it was asked to amplify. 48 is a
    // commanded code rather than 48 dB of gain (see above and the class
    // header); the clamp is what is under test, not the board's response.
    agc.setLnaGainDb(48.0);
    check(agc.agcCeilingDb(kDefaultThresholdUnits) >= 0.0,
          "the referred ceiling never goes negative at full LNA gain");
    check(near(agc.agcCeilingDb(0), 0.0),
          "AGC-T 0 at full LNA gain clamps at zero rather than going negative");

    // Referring UPWARD past the slider's nominal 60 dB top is correct, not an
    // overrun: an operator 12 dB down on the LNA needs 12 dB more AGC gain to
    // hear the same signal at the same level.
    agc.setLnaGainDb(-12.0);                            // the AD9866 floor, real
    check(near(agc.agcCeilingDb(100), 60.0 + 32.0),
          "a 32 dB LNA cut refers AGC-T 100 above the slider's nominal top");
    check(agc.agcCeilingDb(100) <= Hl2DbReference::kAgcCeilingDbMax,
          "the referred ceiling stays inside WDSP's own maximum");

    // The display term must NOT leak into the audio term. fullScaleDbm
    // calibrates a dBm axis; the AGC lives inside the digital chain and never
    // sees dBm, so calibrating the display must not move the heard level.
    agc.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb);
    const double beforeCalibration = agc.agcCeilingDb(kDefaultThresholdUnits);
    agc.setFullScaleDbm(-60.0);
    check(near(agc.agcCeilingDb(kDefaultThresholdUnits), beforeCalibration),
          "calibrating the display does not move the AGC ceiling");
    check(near(agc.offsetDb(), -60.0),
          "...while it does move the display offset");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_dbref_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
