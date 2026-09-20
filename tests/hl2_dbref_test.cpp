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

    // CALIBRATED BY DEFAULT NOW, and the four assertions this replaces were a
    // deliberate tripwire rather than stale expectations. They said: this axis
    // is dBFS wearing a dBm label, we know it, and nothing may quietly change
    // that. One of them guarded the exact regression that caused the absolute
    // form to be REVERTED once -- "default gain leaves the displayed floor
    // exactly where it was".
    //
    // The tripwire is crossed on purpose, and what makes that legitimate is the
    // thing the revert was missing: kFullScaleDbmAtZeroGain is DERIVED from the
    // AD9866 datasheet and the HL2's own input network, rather than being a
    // second arbitrary number. The floor still moves. It now moves to a figure
    // that can be checked.
    //
    // IT HAS ALREADY BEEN WRONG ONCE, by 4 dB, and the way it was wrong is the
    // reason this comment does not cite a confirmation any more. The first
    // draft SUBTRACTED the 2 dB of transformer and filter-board insertion loss
    // instead of adding it, and then quoted DL1YCF's "-34 dBm clipping at
    // +33 dB" as agreeing with the result to the digit. The agreement was the
    // tell, not the evidence: that figure also assumes +33 dB was delivered,
    // and IF the gain folds -- which #5752 left unresolved against the native
    // bit-6-selected RTL path -- a commanded +33 would apply as +1.
    //
    // If this block ever fails again, the question to ask is not "has the
    // arithmetic drifted" but "has the DERIVATION been falsified" -- and the
    // answer belongs beside kFullScaleDbmAtZeroGain, not here.
    check(near(Hl2DbReference::kFullScaleDbmAtZeroGain, 3.0),
          "full scale at 0 dB LNA gain is the derived +3 dBm");
    check(near(ref.fullScaleDbm(), Hl2DbReference::kFullScaleDbmAtZeroGain),
          "a fresh reference carries the derived figure, not 0.0");

    // DERIVED IS NOT CALIBRATED, and this pair is the assertion that keeps the
    // two apart. An earlier draft of this PR moved the default off 0.0 while
    // isCalibrated() was still `m_fullScaleDbm != 0.0`, so the predicate
    // flipped TRUE for a radio nobody has ever measured -- and
    // Hl2Backend::capabilities() publishes it as PanAmplitudeModel::
    // calibratedDbm, which licenses comparing this radio's levels with another
    // station's. The derivation does not license that; a measurement does.
    check(!ref.isCalibrated(),
          "a derived default is NOT a calibration -- nothing has measured this "
          "radio");

    // ...and the setter is what changes the answer. This is the only thing in
    // the class that does, which is what makes the predicate mean provenance
    // rather than "some number is present".
    {
        Hl2DbReference measured;
        measured.setFullScaleDbm(Hl2DbReference::kFullScaleDbmAtZeroGain);
        check(measured.isCalibrated(),
              "applying a measurement calibrates it -- EVEN WHEN THE MEASURED "
              "FIGURE EQUALS THE DERIVED ONE, which is why this is a flag and "
              "not a comparison against the constant");

        Hl2DbReference elsewhere;
        elsewhere.setFullScaleDbm(0.0);
        check(elsewhere.isCalibrated(),
              "and a measurement of 0.0 dBm calibrates it too -- the old "
              "`!= 0.0` predicate got this one wrong in the other direction");
    }

    // P(dBm) = dBFS + fullScale - Glna, absolutely. The class's default gain is
    // the radio's own +20 dB (kLnaDefaultGainDb), which #5752 left standing, so
    // the offset there is 3 - 20 = -17 dB.
    ref.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb);
    check(near(ref.offsetDb(), 3.0 - Hl2DbReference::kDefaultLnaGainDb),
          "offset at the default gain is fullScale - gain");
    check(near(ref.toDbm(-73.0), -90.0),
          "-73 dBFS at the default +20 dB of gain reads -90 dBm");
    check(near(ref.toDbm(-120.0), -137.0),
          "the displayed floor MOVES, to a derived figure rather than an arbitrary one");

    // AND AT 0 dB THE CONSTANT IS THE WHOLE OFFSET, which is what makes it
    // checkable against a signal generator without arithmetic.
    ref.setLnaGainDb(0.0);
    check(near(ref.toDbm(0.0), 3.0),
          "full scale at 0 dB gain reads exactly the derived +3 dBm");

    // From here the reference is deliberately driven to figures that are NOT
    // the derived default; isCalibrated() is true for all of it and is not
    // re-read, because what these cases pin is the arithmetic.
    //
    // THE TRIM'S CASES ARE GONE WITH THE TRIM. setTrimDb() had no caller in
    // src/ -- no UI, no settings key, no automation verb -- so it was dead
    // public surface and Principle IX took it out. These four assertions went
    // with it rather than being kept alive to exercise something nothing can
    // reach: a test is the last place a retired affordance should survive,
    // because it makes the surface look load-bearing to the next reader.
    ref.setLnaGainDb(Hl2DbReference::kDefaultLnaGainDb);

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

    // The documented native range includes +48. This tests the commanded
    // gain arithmetic; actual board response needs independent measurement.
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
    // be the AGC attenuating a signal it was asked to amplify. 48 is
    // the maximum documented commanded gain; this tests the arithmetic clamp,
    // not the board's analog response.
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
    // The display offset is ABSOLUTE (fullScale - gain), so at the default
    // +20 dB of gain a -60 dBm full scale is -80. The assertion above is the
    // one carrying the meaning here -- that calibrating the display leaves the
    // AGC ceiling untouched -- and it still passes, which is the point: the two
    // terms remain separate even though one of them changed form.
    check(near(agc.offsetDb(), -60.0 - Hl2DbReference::kDefaultLnaGainDb),
          "...while it does move the display offset");

    // WHAT THIS CHANGE MOVES AND WHAT IT MUST NOT, across every stored gain the
    // native range documents -- -12..+48, which #5752 left unchanged.
    //
    // The AGC ceiling MUST NOT MOVE. It is built on lnaOffsetDb(), which stayed
    // RELATIVE to the reference gain precisely so that an operator's AGC-T does
    // not shift the moment they connect. That is the compatibility guarantee
    // this loop exists to hold.
    //
    // The display offset MUST MOVE, by exactly the derived full-scale figure.
    // That is the whole change: the absolute term was 0.0 and is now +3 dBm at
    // 0 dB gain, so every stored gain's display offset moves from the old
    // relative (20 - stored) to the absolute (3 - stored) -- a uniform -17 dB
    // shift of the displayed floor onto a number that can be checked against a
    // signal generator.
    check(Hl2DbReference::kDefaultLnaGainDb == 20.0,
          "fresh profiles retain the existing +20 dB reference");
    for (int stored = AetherSDR::hl2::kLnaGainMinDb;
         stored <= AetherSDR::hl2::kLnaGainMaxDb; ++stored) {
        Hl2DbReference after;
        const auto seed = AetherSDR::hl2::connectLna(
            true, true, stored, false, 0, AetherSDR::hl2::kLnaDefaultGainDb,
            AetherSDR::hl2::kLnaGainMinDb, AetherSDR::hl2::kLnaGainMaxDb);
        after.setLnaGainDb(seed.liveDb);

        const double oldCeiling = 39.0 + (20.0 - stored);
        check(near(after.agcCeilingDb(65), oldCeiling),
              "stored gain preserves the old AGC ceiling at 65");

        check(near(after.offsetDb(),
                   Hl2DbReference::kFullScaleDbmAtZeroGain - stored),
              "stored gain moves the display offset onto the derived full scale");
        check(near(after.offsetDb() - (20.0 - stored),
                   Hl2DbReference::kFullScaleDbmAtZeroGain - 20.0),
              "and moves it by the same -17 dB at every stored gain");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_dbref_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
