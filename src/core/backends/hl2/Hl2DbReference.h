#pragma once

#include <algorithm>

#include "core/backends/hl2/Hl2BandMemoryPolicy.h"

namespace AetherSDR::hl2 {

// Owns everything relating raw dBFS to dBm, plus the AGC setpoint that must
// move with it, so the LNA gain and its display offset cannot drift apart.
//
//   * LNA gain: exact while the commanded code is the applied gain. A fold
//     above code 31 was reported (Hermes-Lite2 #177), but ad9866.v at 883a338
//     passes all six bits natively; keep the documented range until reconciled.
//   * fullScaleDbm: derived from the AD9866 datasheet (kFullScaleDbmAtZeroGain),
//     not a per-unit calibration; hl2_dbref_test asserts the step.
//   * AGC ceiling: AGC-T (0..100) -> WDSP max gain, referred to the LNA so a
//     gain change moves no reported dBm and no heard level (HERMES.md 13 item
//     14). fullScaleDbm does not enter it.
//
// One per radio, not per slice: the LNA (AD9866 0x0a[5:0]) feeds all four
// DDCs. AGC-T is per receiver and passed to agcCeilingDb() as an argument.
class Hl2DbReference {
public:
    // Matches Hl2Backend/MetisClient's default LNA setting.
    static constexpr double kDefaultLnaGainDb = kLnaDefaultGainDb;

    // AGC-T units (0..100) -> WDSP max-gain ceiling: 0.6 spans 0..60 dB, putting
    // the default of 65 at 39 dB, measured clean on hardware (see
    // Hl2Backend::setSliceAgc).
    static constexpr double kAgcCeilingDbPerUnit = 0.6;

    // 0 dBFS at the antenna with 0 dB LNA gain, derived (not averaged):
    //   full scale at RxPGA = 48 dB (datasheet)      8.0 mVpp
    //   referred to 0 dB (x251.2)                    2.01 Vpp diff = 0.707 Vrms
    //   into 400 ohm secondary, 0.5/400              1.25 mW = +0.97 dBm at ADC
    //   50->400 ohm transformer (5:14) preserves power
    //   + transformer/N2ADR filter loss ahead of ADC  ~2 dB  => ~+3 dBm
    // Unconfirmed by measurement; DL1YCF's "-34 dBm clip at +33 dB" depends on
    // the open code-45 fold question (#5752). Not the openHPSDR +14 dB per-unit
    // average. The transformer degrades above ~20 MHz (-12.5 dB return loss at
    // 30 MHz), leaving a band-dependent residual on 10 m.
    static constexpr double kFullScaleDbmAtZeroGain = 3.0;

    // Upper bound on the referred ceiling. Referring may exceed the slider's
    // nominal 60 dB (cutting the LNA 12 dB asks for 12 dB more AGC gain); it
    // must not run away or go negative (a negative ceiling would attenuate).
    static constexpr double kAgcCeilingDbMax = 120.0;

    // Gain we commanded on the AD9866 LNA, in dB.
    void setLnaGainDb(double db) noexcept { m_lnaGainDb = db; }
    double lnaGainDb() const noexcept { return m_lnaGainDb; }

    // Applies a measured full-scale figure, replacing the derived default. The
    // only thing that makes isCalibrated() true; not a way to nudge the default.
    void setFullScaleDbm(double dbm) noexcept
    {
        m_fullScaleDbm = dbm;
        m_fullScaleMeasured = true;
    }

    // No operator trim: offsetDb() is exactly fullScaleDbm - lnaGainDb. A trim
    // lands together with a control and persistence that reach it.
    double fullScaleDbm() const noexcept { return m_fullScaleDbm; }

    // True only once setFullScaleDbm applied a measurement. Carried as a flag
    // because provenance is not recoverable from the value (a real measurement
    // can equal the derived +3.0). It feeds PanAmplitudeModel::calibratedDbm,
    // which licenses cross-station comparison, so a datasheet derivation must
    // not set it. Nothing in src/ calls setFullScaleDbm, so this is false today.
    bool isCalibrated() const noexcept { return m_fullScaleMeasured; }

    // The gain the AGC ceiling is referred to: the shipped LNA default, a
    // constant on purpose. Only lnaOffsetDb() reads it. A fixed reference keeps
    // AGC-T at 0.6*T + (default - stored) for every stored gain (pinned by
    // hl2_dbref_test); a moving one would shift heard AGC-T on unrelated events.
    static constexpr double kReferenceLnaGainDb = kDefaultLnaGainDb;

    // The whole point: subtracting the gain we applied is what keeps a signal
    // of constant strength reading the same dBm across a gain change.
    double toDbm(double dbfs) const noexcept
    {
        return dbfs + offsetDb();
    }

    // Offset form for a whole spectrum frame, absolute:
    // P(dBm) = dBFS + fullScale - Glna. The absolute floor is only meaningful
    // because kFullScaleDbmAtZeroGain is derived, so the two go together.
    // isCalibrated() stays false until a bench measurement.
    double offsetDb() const noexcept
    {
        return m_fullScaleDbm - m_lnaGainDb;
    }

    // The LNA term alone, relative to kReferenceLnaGainDb. Only the AGC uses it:
    // its invariant is a difference (constant heard level across a gain change,
    // and the plain 0.6-per-unit map at the reference gain), so an absolute form
    // would shift every operator's AGC-T on connect.
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
