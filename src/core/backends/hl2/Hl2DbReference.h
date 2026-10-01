#pragma once

#include <algorithm>

#include "core/backends/hl2/Hl2BandMemoryPolicy.h"

namespace AetherSDR::hl2 {

// The single owner of everything that relates a raw dBFS number to a dBm one,
// and of the one audio-chain setpoint that has to move with it.
//
// ONE OBJECT so the LNA gain and its display offset can never drift apart:
// every gain change (manual or automatic) shifts the absolute reference by
// exactly the gain step, and without an equal and opposite offset the trace
// jumps and the waterfall paints a false band.
//
// Three terms:
//
//   * LNA gain — exact as far as the commanded code is the applied gain, so
//     within that range a gain change provably cannot move a reported dBm. A
//     fold above code 31 was reported on one board (softerhardware/Hermes-Lite2
//     #177) but ad9866.v at 883a338 passes all six bits in native format, as
//     AetherSDR sets it; keep the documented range until that is reconciled. A
//     board-specific correction would need a qualified mapping shared with the
//     reported gain.
//   * fullScaleDbm — DERIVED from the AD9866 datasheet and input network (see
//     kFullScaleDbmAtZeroGain), not a per-unit calibration. The absolute form
//     moves the displayed noise floor onto a checkable figure;
//     hl2_dbref_test asserts the step.
//   * AGC ceiling — AGC-T (0..100) becomes WDSP's maximum gain, a setpoint
//     about the antenna signal that WDSP applies after the LNA. Referring it by
//     the LNA term means a gain change moves no reported number AND no heard
//     level (needed by the RF-gain regulator, HERMES.md 13 item 14).
//     fullScaleDbm does not enter it: the AGC never sees dBm.
//
// ONE PER RADIO, NOT PER SLICE. The LNA (AD9866 0x0a[5:0]) sits in front of all
// four DDCs and fullScaleDbm is a board property. AGC-T is per receiver
// (Hl2Backend::Receiver::agcThresholdDb) and is passed to agcCeilingDb() as an
// argument. Split this only if the hardware gains a per-DDC front end.
class Hl2DbReference {
public:
    // Matches Hl2Backend/MetisClient's default LNA setting.
    static constexpr double kDefaultLnaGainDb = kLnaDefaultGainDb;

    // Operator AGC-T units (0..100) -> WDSP maximum-gain ceiling in dB. 0.6
    // spans 0..60 dB, which puts the default of 65 at 39 dB -- measured clean
    // on live hardware where the previous 0..100 mapping had the DEFAULT
    // sitting 25 dB past the clipping point. See Hl2Backend::setSliceAgc for
    // that measurement. This is a WDSP-range fact, not an HL2 one, but it
    // belongs here because the ceiling it produces is referred to this object.
    static constexpr double kAgcCeilingDbPerUnit = 0.6;

    // WHAT 0 dBFS IS AT THE ANTENNA, with 0 dB of LNA gain: +3 dBm. Derived from the
    // AD9866 datasheet and the HL2 input network, not averaged:
    //
    //   full scale at RxPGA = 48 dB (datasheet)          8.0 mVpp
    //   referred to 0 dB -- 48 dB is x251.2              2.01 Vpp differential
    //   as RMS                                           0.707 Vrms
    //   into the 400 ohm secondary, V^2/R = 0.5/400      1.25 mW
    //   in dBm, AT THE CONVERTER                         +0.97 dBm
    //   the 50->400 ohm input transformer (5:14) preserves power
    //   transformer + N2ADR filter board insertion loss  about 2 dB
    //   ...which sits BETWEEN the antenna and the converter,
    //   so the antenna must deliver that much MORE        +2 dB
    //   -------------------------------------------------------------
    //   full scale at the antenna, 0 dB LNA gain         about +3 dBm
    //
    // Loss ahead of the converter raises the antenna-referred full scale:
    // P_ant = P_adc + 2 dB.
    //
    // Not independently confirmed. DL1YCF's "-34 dBm clipping at +33 dB" cannot
    // confirm or refute it: it depends on whether a commanded +33 (code 45) was
    // applied or folded `& 0x1F`, which is open (#5752, Hermes-Lite2 #177). This
    // figure is at 0 dB gain and does not depend on that. A bench measurement (known
    // level at a known applied gain, against the clip counter) would settle it.
    //
    // Not the openHPSDR +14 dB, which is a per-unit average — a different kind of
    // number. The input transformer degrades above ~20 MHz (IN3OTD: -12.5 dB return
    // loss at 30 MHz), so a band-dependent residual remains on 10 m.
    static constexpr double kFullScaleDbmAtZeroGain = 3.0;

    // WDSP's own default maximum gain, used here only as the bound on what
    // referring the ceiling may produce. Referring can push the ceiling ABOVE
    // the slider's nominal 60 dB top -- an operator who cut the LNA 12 dB is
    // asking for 12 dB more AGC gain to hear the same signal at the same level,
    // and that is the correct answer, not an overrun. What it must never do is
    // run away, and it must never go negative: a ceiling below zero would be
    // the AGC attenuating a signal it was asked to amplify.
    static constexpr double kAgcCeilingDbMax = 120.0;

    // Gain we commanded on the AD9866 LNA, in dB.
    void setLnaGainDb(double db) noexcept { m_lnaGainDb = db; }
    double lnaGainDb() const noexcept { return m_lnaGainDb; }

    // APPLY A MEASURED full-scale figure, replacing the derived default.
    //
    // This setter is the ONLY thing that makes isCalibrated() true, and that
    // is now its whole contract rather than a side effect of the value it
    // happens to write. Calling it says "somebody measured this radio"; it is
    // not the way to nudge the derived figure.
    //
    // The derived default is still what the object starts with, so calibrating
    // is a REPLACEMENT, not an addition -- there is no trim term to combine
    // with (see below).
    void setFullScaleDbm(double dbm) noexcept
    {
        m_fullScaleDbm = dbm;
        m_fullScaleMeasured = true;
    }

    // THE OPERATOR TRIM IS NOT HERE, AND THAT IS A DELIBERATE SUBTRACTION.
    //
    // #5740 proposed three things and this class shipped all three: the derived
    // constant, the absolute form, and a bounded +/-3 dB operator trim. Review
    // pointed out that `setTrimDb` had NO CALLER anywhere in src/ -- no UI, no
    // settings key, no automation verb reached it. That is dead public surface,
    // and Principle IX is "Surface Only What Survives".
    //
    // So it is removed rather than justified. It lands with the control that
    // sets it, in the change that gives an operator a way to reach it, and that
    // change can carry its own persistence question -- a trim that does not
    // survive a restart is a worse affordance than none. Until then
    // offsetDb() is fullScaleDbm - lnaGainDb exactly, with nothing in it that
    // nothing can move.
    double fullScaleDbm() const noexcept { return m_fullScaleDbm; }

    // CALIBRATED MEANS A MEASUREMENT WAS APPLIED. It does not mean "a number
    // is present", and the difference is the whole of this predicate.
    //
    // IT USED TO BE `m_fullScaleDbm != 0.0`, and that worked only for as long
    // as the field started at zero. The derived default above is +3.0, so the
    // same test answered TRUE on a radio nobody has ever measured -- and
    // Hl2Backend::capabilities() publishes this straight into
    // PanAmplitudeModel::calibratedDbm, whose documented meaning
    // (RadioCapabilities.h) is that a level from this radio MAY be compared
    // with another station's, published as a spot, or used as an absolute
    // threshold. A derivation from a datasheet does not earn that; only a
    // measurement against a reference source does. The derived figure is a
    // much better ZERO POINT than 0.0 was, and it is still not a calibration.
    //
    // SNIFFING THE VALUE CANNOT WORK, which is why this holds a flag instead.
    // `m_fullScaleDbm != kFullScaleDbmAtZeroGain` would be the smaller edit and
    // repeats the original bug one constant along: a genuine measurement that
    // lands on +3.0 dBm would read UNCALIBRATED, exactly as a genuine
    // measurement of 0.0 dBm did before. Provenance is not recoverable from a
    // double, so it is carried rather than inferred.
    //
    // AND NOT AN isDerived()/isCalibrated() PAIR. The reference is derived
    // whenever it is not measured, so the second accessor is the negation of
    // the first with nothing to call it -- the same dead public surface
    // Principle IX took setTrimDb out for, two paragraphs above. If a UI ever
    // has to say "derived" rather than "uncalibrated", it lands with that UI.
    //
    // NOTHING IN src/ CALLS setFullScaleDbm TODAY, so this is false in
    // production and every comment that says the HL2 axis is dBFS wearing a
    // dBm label stays true. That is the honest answer until the bench
    // measurement named beside kFullScaleDbmAtZeroGain is made.
    bool isCalibrated() const noexcept { return m_fullScaleMeasured; }

    // THE GAIN THE AGC CEILING IS REFERRED TO: the shipped LNA default, and a
    // CONSTANT, not a setting.
    //
    // There used to be a setReferenceLnaGainDb() here and a member behind it.
    // Nothing in src/ or tests/ ever called it (#5625 review), so the
    // "reference" was this constant with a setter around it. It is removed
    // rather than wired, because there is nothing it would be right to wire
    // it to:
    //
    //   * Only lnaOffsetDb() -- and so only the AGC ceiling -- reads it. The
    //     display moved to the absolute offsetDb() and no longer has a
    //     reference gain at all; the comment that stood here ("at this gain
    //     the offset is zero, so the reported number is raw dBFS") described
    //     the retired relative display form.
    //   * The ceiling's compatibility guarantee IS a fixed reference: at the
    //     shipped default the operator's AGC-T maps through the plain
    //     0.6-per-unit table, and every stored gain gets 0.6*T + (default -
    //     stored), which hl2_dbref_test pins across the whole range. A
    //     reference that moved -- to the connect-time gain, a band's stored
    //     gain, or the auto-gain baseline -- would move every operator's
    //     heard AGC-T on events they did not associate with it.
    //   * Nothing an operator does can move the shipped default (the #5829 fix took
    //     the persisted "defaultDb" key out for exactly that reason), so a
    //     setter here would be a second source of truth for a value no action
    //     reaches -- the same dead public surface Principle IX removed
    //     setTrimDb for, above.
    //
    // If a per-radio reference is ever needed, it lands with the thing that
    // sets it and the test that says why, not as a setter nobody calls.
    static constexpr double kReferenceLnaGainDb = kDefaultLnaGainDb;

    // The whole point: subtracting the gain we applied is what keeps a signal
    // of constant strength reading the same dBm across a gain change.
    double toDbm(double dbfs) const noexcept
    {
        return dbfs + offsetDb();
    }

    // Offset form, for applying to a whole spectrum frame without a call per bin.
    // ABSOLUTE, not referred to a nominal gain: P(dBm) = dBFS + fullScale - Glna.
    //
    // The first version of this class subtracted the gain absolutely, and it
    // was REVERTED for a reason its own comment records: it "silently moved the
    // whole displayed noise floor by 20 dB, from about -120 to about -140,
    // because the default LNA gain is 20 dB. Neither number is calibrated, so
    // that shift bought nothing."
    //
    // That objection was correct and it is what kFullScaleDbmAtZeroGain
    // removes. The floor still moves, but it moves to a figure derived from the
    // AD9866 datasheet and the HL2's own input network, instead of from one
    // arbitrary number to another. The two halves cannot be separated: this
    // form filed without the constant fails on exactly the old grounds.
    //
    // NOT "CONFIRMED INDEPENDENTLY", which this sentence used to claim. The
    // only cross-check ever offered was DL1YCF's, and it is WITHDRAWN for the
    // reasons set out beside kFullScaleDbmAtZeroGain. The derivation stands on
    // the datasheet alone, and isCalibrated() is false until a bench
    // measurement says otherwise.
    double offsetDb() const noexcept
    {
        return m_fullScaleDbm - m_lnaGainDb;
    }

    // The LNA term alone, RELATIVE to the reference gain -- what has to be
    // undone, wherever it is undone.
    //
    // STILL RELATIVE, DELIBERATELY, AND ONLY THE AGC USES IT NOW. The display
    // path moved to an absolute form when fullScaleDbm became a real figure
    // (see offsetDb), but the AGC's invariant is a DIFFERENCE: a constant
    // antenna signal must stay at a constant heard level across a gain change,
    // and at the reference gain the operator must see exactly the 0.6-per-unit
    // map they saw before this term existed. An absolute form here would move
    // every operator's AGC-T the moment they connected.
    double lnaOffsetDb() const noexcept
    {
        return kReferenceLnaGainDb - m_lnaGainDb;
    }

    // The operator's AGC-T, referred to this reference. Same invariant as the
    // display: a constant antenna signal keeps a constant heard level across a
    // gain change, because the ceiling moves down by exactly what the LNA moved
    // up. At the reference gain this is the plain 0.6-per-unit map, so an
    // operator who never touches RF gain sees no change from before this term
    // existed.
    double agcCeilingDb(int thresholdUnits) const noexcept
    {
        const double base = static_cast<double>(thresholdUnits) * kAgcCeilingDbPerUnit;
        return std::clamp(base + lnaOffsetDb(), 0.0, kAgcCeilingDbMax);
    }

private:
    double m_lnaGainDb = kDefaultLnaGainDb;
    double m_fullScaleDbm = kFullScaleDbmAtZeroGain;

    // DERIVED UNTIL SOMEBODY MEASURES IT. Only setFullScaleDbm sets this, and
    // nothing clears it: a radio does not become uncalibrated again.
    bool m_fullScaleMeasured = false;
};

}  // namespace AetherSDR::hl2
