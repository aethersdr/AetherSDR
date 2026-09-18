#pragma once

#include <algorithm>

#include "core/backends/hl2/Hl2BandMemoryPolicy.h"

namespace AetherSDR::hl2 {

// The single owner of everything that relates a raw dBFS number to a dBm one,
// and of the one audio-chain setpoint that has to move with it.
//
// WHY THIS IS ONE OBJECT
//
// Every LNA gain change shifts the absolute signal reference by exactly the
// same amount. If the panadapter displays dBm and the gain moves, the whole
// trace jumps and the waterfall paints a horizontal band that reads as a real
// on-air event. The fix is to apply an equal and opposite offset in the display
// chain -- and the only way to guarantee the two can never drift apart is to
// keep the gain value and its offset in the same object, which is what this is.
//
// This matters before there is any automatic RF AGC, because a manual gain
// change has the identical problem.
//
// WHAT IS AND IS NOT CALIBRATED
//
// The LNA term is exact AS FAR AS THE COMMANDED CODE IS THE APPLIED GAIN: it is
// the gain we ourselves commanded, so removing it is arithmetic, not
// estimation. Within that range a gain change provably cannot move a reported
// dBm value.
//
// A fold above code 31 was reported on one board in upstream issue #177.
// Its scope is unresolved: ad9866.v at 883a338 passes all six gain bits when
// the native-format flag is set, as AetherSDR sets it. Keep the documented
// range and existing reference until the measured behavior is reconciled with
// that command path; do not reinterpret stored gains from this observation.
// https://github.com/softerhardware/Hermes-Lite2/issues/177
//
// This object knows the commanded gain, not the analog response of a board.
// Any future hardware-specific correction needs a qualified mapping shared
// with the reported gain and separate validation of the display and AGC paths.
//
// THE ABSOLUTE TERM IS NOW DERIVED, AND THIS PARAGRAPH USED TO ARGUE THE
// OPPOSITE. It said fullScaleDbm "is NOT calibrated here ... so it defaults to
// 0.0 and the gain term is measured RELATIVE to a reference gain, not
// absolutely", and then explained why the relative form mattered. Every word of
// that was true of the tree that carried it and none of it is true now. Left
// standing it would have been the strongest argument against the change it sits
// inside -- flagged in review, and a header arguing against its own class is
// worse than one that says nothing.
//
// WHAT CHANGED IS THE OBJECTION, NOT THE STANDARD. The old reasoning turned on
// "neither number is calibrated, so an absolute form buys nothing and only
// moves the floor" -- and it was right, because the alternative on offer was a
// per-unit calibration nobody had. It is not the alternative here.
// fullScaleDbm is DERIVED from the AD9866 datasheet and the HL2's own input
// network (see kFullScaleDbmAtZeroGain below), and a derived figure is not a
// per-unit one. Quisk and SparkSDR build per-unit tables for the analogous TX
// power question; this is a different kind of number and the comparison does
// not carry.
//
// The concrete objection the old form raised is also answered rather than
// waved through: yes, the absolute form moves the displayed noise floor, and
// that is the point -- it moves it ONTO a figure that can be checked, from one
// that could not. What it must never do is move without saying so, which is
// why the step is asserted in hl2_dbref_test rather than left to arrive.
//
// THE THIRD TERM: THE AGC CEILING, WHICH THE OPERATOR HEARS
//
// The display half of this is the half you can see, and it was built first.
// The AGC-T half is the half you hear, and it has the same cause.
//
// The operator's AGC-T is a 0..100 slider that becomes a WDSP MAXIMUM GAIN in
// dB -- the most gain the AGC is allowed to apply, which is what decides how
// far down into the noise it will chase a weak signal. It is a setpoint about
// the signal AT THE ANTENNA, but WDSP applies it to a signal that has already
// been through the LNA. Raise the LNA 6 dB and the same antenna signal arrives
// at the AGC 6 dB hotter, so the same ceiling now lets the AGC amplify 6 dB
// further into the noise than the operator asked for -- the band floor comes
// up in the headphones and the operator turns the AGC-T down to compensate.
// Lower the LNA and weak signals fall out from under the ceiling instead.
//
// So the ceiling is referred to the reference the same way the display is:
// subtract the LNA term, and a gain change moves no reported number AND no
// heard level. This is the dependency item 14's RF-gain regulator needs --
// that regulator steps the RxPGA 3-6 dB several times a day, and the display
// half was already safe. This is the half that was not.
//
// fullScaleDbm deliberately does NOT enter the ceiling. It is the dBFS->dBm
// calibration of a DISPLAY axis; the AGC lives entirely inside the digital
// chain and never sees dBm. The two consumers share the LNA term and differ in
// the calibration term, which is an argument for keeping the terms in one
// object and deriving each consumer's combination here, rather than handing
// out a single "offset" and hoping both callers apply it correctly.
//
// WHY THIS IS NOT ONE OBJECT PER SLICE
//
// The backlog row that asked for this (docs/HERMES.md 13, item 12) says "one
// dB-reference object per slice". Built literally that would be wrong on this
// radio, and the reason is worth stating because the row will outlive it.
//
// Of the three terms, TWO ARE PROPERTIES OF THE RADIO, not of a slice:
//
//   * The LNA gain is one AD9866 field (0x0a[5:0]) in front of ALL four DDCs.
//     There is no per-receiver RF gain to hold. N copies of one number is
//     precisely the drift this class was created to make impossible.
//   * fullScaleDbm is a property of the board, the ADC reference and the front
//     end -- again one per radio, shared by every DDC.
//
// ONE IS GENUINELY PER SLICE: the AGC-T, an operator judgement about one
// receiver's audio, which two receivers on different bands may legitimately
// disagree about. It lives with the rest of that receiver's state, in
// Hl2Backend::Receiver::agcThresholdDb, and is passed to agcCeilingDb() at the
// point of use.
//
// So the reference is ONE object per radio, and the per-slice quantity is an
// ARGUMENT to it rather than a copy inside it. That gives the row what it was
// actually after -- a slice's AGC ceiling that moves with the reference -- with
// no second copy of a gain that physically cannot differ between slices.
//
// If the hardware ever changes (per-DDC front-end gain, or per-slice
// calibration), split it then, and the split will be forced by a real
// difference rather than by a sentence.
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

    // WHAT 0 dBFS IS AT THE ANTENNA, with 0 dB of LNA gain: +3 dBm.
    //
    // DERIVED, NOT AVERAGED, and that distinction is the whole reason this is a
    // constant rather than a per-unit calibration. Every step is from the
    // AD9866 datasheet and the HL2's own input network:
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
    // THE SIGN OF THE LAST TERM WAS WRONG IN THE FIRST DRAFT, which subtracted
    // the insertion loss and arrived at -1 dBm. Loss ahead of the converter
    // makes the antenna-referred full-scale point HIGHER, not lower: P_adc =
    // P_ant - 2 dB, so P_ant = P_adc + 2 dB. The old figure read every signal
    // 4 dB weak. Caught by aethersdr-agent on #5753.
    //
    // NO INDEPENDENT CONFIRMATION, and the one this file used to cite is
    // WITHDRAWN. DL1YCF's "-34 dBm clipping at +33 dB of gain" arithmetically
    // gives -1 dBm and was quoted here as agreeing "to the digit" -- with a
    // figure now known to be 4 dB out, which is the tell. It also assumes +33
    // dB was DELIVERED, and on one measured unit it was not: a commanded +33
    // is code 45, and if that code folds `& 0x1F` it lands on 13, i.e. +1 dB
    // applied, which makes the same measurement give -33 dBm instead.
    //
    // WHETHER IT FOLDS IS OPEN. #5752 looked at exactly this and declined to
    // treat the fold as general: it clamped connect parameters without changing
    // the native range, left -12..+48 and the +20 dB default standing, and
    // recorded that the single-unit observation in softerhardware/Hermes-Lite2
    // #177 "remains unresolved against the native bit-6-selected RTL path".
    // So this cross-check is indeterminate for two independent reasons --
    // commanded-versus-applied cannot be established from the published figure,
    // and the fold that would decide it is itself unsettled. It confirms
    // nothing in either direction and is recorded here only so nobody
    // re-derives it.
    //
    // NONE OF WHICH TOUCHES THE DERIVATION BELOW. kFullScaleDbmAtZeroGain is a
    // figure AT 0 dB LNA gain, taken from the AD9866 datasheet and the input
    // network; it does not depend on what the gain register does above code 31.
    //
    // WHAT WOULD SETTLE IT is a bench measurement on this radio: a known level
    // into the antenna port at a known APPLIED gain, read against the ADC clip
    // counter. That is receive-only and wants a calibrated source.
    //
    // NOT THE openHPSDR FIGURE. piHPSDR and deskHPSDR carry +14 dB, and their
    // own notes describe it as "average, varies per unit". This is not that,
    // and a reader comparing the two should know they are different KINDS of
    // number rather than two estimates of one.
    //
    // WHAT IT DOES NOT COVER. The input transformer runs away above ~20 MHz --
    // IN3OTD measured return loss falling to -12.5 dB at 30 MHz -- so on 10 m a
    // band-dependent residual sits on top of the ~1 dB this buys. A per-band
    // table could take that later; it is not a reason to leave the reference at
    // zero, which is what "uncalibrated" actually meant here.
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

    // The gain the uncalibrated scale is referred to. At this gain the offset
    // is zero, so the reported number is raw dBFS. Defaults to the backend's
    // default LNA setting, which is what keeps the displayed floor where the
    // operator has always seen it.
    void setReferenceLnaGainDb(double db) noexcept { m_referenceLnaGainDb = db; }
    double referenceLnaGainDb() const noexcept { return m_referenceLnaGainDb; }

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
        return m_referenceLnaGainDb - m_lnaGainDb;
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
    double m_referenceLnaGainDb = kDefaultLnaGainDb;
    double m_fullScaleDbm = kFullScaleDbmAtZeroGain;

    // DERIVED UNTIL SOMEBODY MEASURES IT. Only setFullScaleDbm sets this, and
    // nothing clears it: a radio does not become uncalibrated again.
    bool m_fullScaleMeasured = false;
};

}  // namespace AetherSDR::hl2
