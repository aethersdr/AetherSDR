#pragma once

#include <cstdint>

namespace AetherSDR {

class RadioSettingsScope;

// Manual frequency calibration for the Hermes-Lite 2.
//
// WHY THIS EXISTS AT ALL. The HL2 tunes off a free-running 38.4 MHz crystal
// multiplied to 76.8 MHz by the VersaClock. That 76.8 MHz is baked into the
// BITSTREAM: radio.v computes the NCO phase word as
//
//     freqcomp = f_Hz * M2 + 2^24,   M2 = 2^57 / 76'800'000   (a localparam)
//     phase    = freqcomp[56:25]
//
// so there is no register anywhere in the HL2 map — and no function in the
// HL2's own tooling — that can be told the crystal's actual error. Correction
// is the host's job or it does not happen. (Quisk reaches the same conclusion
// from the other direction, carrying the measured clock as `rx_udp_clock` and
// reconciling it against the FPGA constant in Freq2Phase().)
//
// THE ONE RESULT THIS FILE RESTS ON. With a true clock of 76.8 MHz * (1 + e):
//
//   1. the hardware LO lands at U*(1+e) for a commanded U;
//   2. samples arrive at 48000*(1+e) but every stage above — the WDSP shift,
//      the demodulator, the panadapter axis — labels them 48000, so a real
//      baseband offset b READS as b/(1+e);
//   3. displayed frequency = U + (F - U(1+e))/(1+e) = F/(1+e).
//
// The whole frequency scale is off by exactly 1/(1+e) — independent of where
// the NCO sits and of how much software shift is in play. That is what makes a
// single multiplicative scalar sufficient, and it is why this is NOT a per-band
// offset table: the error is fractional, not additive. A "1 Hz" correction that
// is right on 10 MHz is wrong everywhere else.
//
// SIGN CONVENTION, stated once so every caller can stop guessing:
//
//   ppb > 0  =>  clock FAST  =>  signals appear LOW before correction.
//
// Matching the vocabulary of the Flex path already in RadioSetupDialog
// ("radio set freq_error_ppb"), so the two families' calibration UIs do not
// disagree about what a positive number means.
class Hl2FreqCal {
public:
    // Nominal AD9866 sample clock. The value the gateware assumes; the whole
    // point of this class is that the real one differs.
    static constexpr double kNominalClockHz = 76'800'000.0;

    // +/- 50 ppm. A stock HL2 lands inside +/- 10 ppm and a good one inside
    // 0.2 ppm; this leaves room for a bad crystal without letting a typo command
    // something absurd (Principle VII — validate rather than trust).
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
