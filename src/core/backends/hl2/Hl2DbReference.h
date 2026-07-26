#pragma once

namespace AetherSDR::hl2 {

// The single owner of everything that relates a raw dBFS number to a dBm one.
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
// The LNA term is exact: it is the gain we ourselves commanded, so removing it
// is arithmetic, not estimation. A gain change provably cannot move a reported
// dBm value.
//
// The absolute term (fullScaleDbm -- what 0 dBFS corresponds to at the antenna
// with 0 dB of LNA gain) is NOT calibrated here. It is a per-unit property of
// the board, the ADC reference and the front end, and none of the HL2 oracles
// state a figure for it; Quisk and SparkSDR both build per-unit calibration
// tables for the analogous TX power question rather than quoting a constant.
//
// So it defaults to 0.0 and the gain term is measured RELATIVE to a reference
// gain, not absolutely. At the default LNA setting the offset is exactly zero
// and this backend reports precisely what it reported before this type existed:
// dBFS on a dBm-labelled axis. That is still wrong, but it is the SAME wrong,
// in one labelled place with a setter, instead of being invisible.
//
// The relative form matters. Subtracting the gain ABSOLUTELY would have been
// just as defensible in theory and was what the first version of this did --
// and it silently moved the whole displayed noise floor by 20 dB, from about
// -120 to about -140, because the default LNA gain is 20 dB. Neither number is
// calibrated, so that shift bought nothing and would have looked to the
// operator exactly like the regression this class exists to prevent.
class Hl2DbReference {
public:
    // Matches Hl2Backend/MetisClient's default LNA setting.
    static constexpr double kDefaultLnaGainDb = 20.0;

    // Gain we commanded on the AD9866 LNA, in dB.
    void setLnaGainDb(double db) noexcept { m_lnaGainDb = db; }
    double lnaGainDb() const noexcept { return m_lnaGainDb; }

    // What 0 dBFS means at the antenna with 0 dB LNA gain. Needs per-unit
    // calibration to be meaningful; 0.0 means "uncalibrated, reporting dBFS".
    void setFullScaleDbm(double dbm) noexcept { m_fullScaleDbm = dbm; }
    double fullScaleDbm() const noexcept { return m_fullScaleDbm; }
    bool isCalibrated() const noexcept { return m_fullScaleDbm != 0.0; }

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
    double offsetDb() const noexcept
    {
        return m_fullScaleDbm - (m_lnaGainDb - m_referenceLnaGainDb);
    }

private:
    double m_lnaGainDb = kDefaultLnaGainDb;
    double m_referenceLnaGainDb = kDefaultLnaGainDb;
    double m_fullScaleDbm = 0.0;
};

}  // namespace AetherSDR::hl2
