#pragma once

#include <cstdint>

namespace AetherSDR {

class RadioSettingsScope;

// Manual frequency calibration for the Hermes-Lite 2. The 76.8 MHz clock is a
// bitstream constant (radio.v: freqcomp = f_Hz * 2^57/76'800'000 + 2^24, phase
// = freqcomp[56:25]); no register takes the crystal error, so the host corrects.
// With a true clock of 76.8 MHz*(1+e) the LO lands at U(1+e) and baseband reads
// b/(1+e), so displayed = F/(1+e) everywhere: one multiplicative scalar, not a
// per-band offset. Sign (matches Flex freq_error_ppb): ppb > 0 => clock fast =>
// signals appear low before correction.
class Hl2FreqCal {
public:
    // Nominal AD9866 sample clock. The value the gateware assumes; the whole
    // point of this class is that the real one differs.
    static constexpr double kNominalClockHz = 76'800'000.0;

    // +/- 50 ppm. A stock HL2 lands inside +/- 10 ppm and a good one inside
    // 0.2 ppm; this leaves room for a bad crystal without letting a typo command
    // something absurd.
    static constexpr int kMinPpb = -50'000;
    static constexpr int kMaxPpb =  50'000;

    // ---- math (pure, no storage — this is the part the tests pin) ----

    // The scale applied to every commanded frequency: k = 1/(1 + ppb/1e9).
    // Command trueHz * k and the radio lands on trueHz.
    static double scaleForPpb(int ppb) noexcept;

    // Recover the error from a zero-beat. The operator tunes a reference of
    // known true frequency and reads the dial value at which it nulls; from
    // "displayed = F/(1+e)" above, 1+e = referenceHz/dialledHz.
    //
    // Returns 0 for a non-positive or absurd input rather than propagating a
    // nonsense scale into the tuning path. Result is clamped to [kMinPpb,
    // kMaxPpb]: an operator who zero-beats the wrong signal (a harmonic, the
    // wrong sideband) would otherwise commit a calibration that moves every
    // band by kilohertz.
    static int ppbFromZeroBeat(double referenceHz, double dialledHz) noexcept;

    static int clampPpb(int ppb) noexcept;

    // The real master clock implied by a calibration, for display. Derived, not
    // stored — one number is the truth and everything else is a view of it.
    static double effectiveClockHz(int ppb) noexcept;

    // How far off the radio would be at this frequency with NO correction.
    // The readout that makes ppb mean something to an operator: "-182 ppb" is
    // abstract, "-5.2 Hz at 28.5 MHz" is not.
    static double errorHzAt(double rfHz, int ppb) noexcept;

    // ---- the two conversions the tuning path uses ----

    // The value to WRITE to an NCO register (0x01 TX, 0x02+ RX) so the hardware
    // oscillator lands on trueHz. Clamped at 0: the register is unsigned, and a
    // negative frequency is a bug upstream, not something to wrap around.
    static std::uint32_t ncoCommandHz(double trueHz, double scale) noexcept;

    // The shift to hand the DSP so the slice lands on sliceTrueHz, given the
    // NCO command ALREADY ROUNDED to its integer register value.
    //
    // Computing this against ncoCommand rather than against the ideal NCO is
    // what makes the pair exact: the realised frequency is
    //   (ncoCommand + shift) * (1+e) = sliceTrueHz * k * (1+e) = sliceTrueHz,
    // so the register's 1 Hz quantisation cancels completely instead of leaking
    // into the audio. Do not "simplify" this to (sliceTrueHz - ncoTrueHz)*scale.
    static double dspShiftHz(double sliceTrueHz, std::uint32_t ncoCommand,
                             double scale) noexcept;

    // ---- per-radio persistence ----
    //
    // Keyed by RADIO, not globally. The number describes one physical crystal;
    // an operator with two HL2s must not have the second one's calibration
    // silently applied to the first. Stored as a feature document in the
    // radio-scoped store (RFC #4603), the same mechanism Hl2Discovery uses for
    // client-side nicknames.
    static constexpr const char* kFeature = "Calibration";
    static constexpr const char* kFieldPpb = "freqErrorPpb";
    static constexpr int kSchemaVersion = 1;

    // 0 when never calibrated, when the document is absent, and when it holds
    // something unparseable — all the same answer: we have no calibration, so
    // tune uncorrected rather than acting on a truncated settings file.
    static int loadPpb(const RadioSettingsScope& scope);
    static void savePpb(const RadioSettingsScope& scope, int ppb);
};

}  // namespace AetherSDR
