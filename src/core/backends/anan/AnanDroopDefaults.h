#pragma once

#include "core/backends/anan/AnanDroopCorrection.h"

namespace AetherSDR::anan {

// Compiled-in droop-correction defaults for the ANAN-G2, so a radio that has
// never been calibrated still gets a corrected panadapter out of the box. A
// per-radio bench calibration always overrides these (AnanBackend::
// connectRadio() seeds these first, then overlays whatever the in-app sweep
// persisted for THIS radio).
//
// DERIVED FROM THE GATEWARE, NOT MEASURED, which is what makes shipping one
// curve to every unit defensible. The Saturn DDC0 chain is
//
//     ADC 122.88 MSPS -> CIC   6 stages, differential delay 1, R = 10..320
//                     -> FIR   1024 taps, decimate by 8, 18-bit coefficients
//                     -> output at 122.88e6 / (R * 8)
//
// which reproduces all six P2 rates exactly (R=320 -> 48 ksps ... R=10 ->
// 1536 ksps). Both stages are fully specified in the Saturn FPGA sources, so
// the correction is a computation, not a per-unit measurement: it is a
// property of the bitstream, identical across every radio running it.
//
// Note what the roll-off actually is, because it is easy to get wrong: across
// the DDC OUTPUT band the CIC contributes at most 0.34 dB and varies by
// 0.003 dB between the fastest and slowest rate. Essentially all of the
// visible droop is the anti-alias FIR's transition band -- flat to +/-0.2 dB
// out to 8% in from each edge, then -6 dB at 5% in, -15 dB at 4%, -30 dB at
// 3%, -54 dB at 2%. Below about 2% there is no signal left to recover, which
// is why that zone is faded by applyEdgeFade() rather than corrected.
//
// ONE TABLE COVERS ALL SIX RATES. The FIR sees the same normalised frequency
// regardless of the CIC's decimation, so the curve is rate-independent by
// construction; the residual CIC term differs by 0.003 dB across the whole
// range. defaultDroopTableForRate() therefore returns the same table for every
// valid rate, and exists to reject invalid ones rather than to select.
//
// Validated against hardware rather than asserted: measured on an ANAN-G2
// (gateware 27) over the region the correction actually works in, the derived
// curve agrees to a mean bias of <= 0.13 dB at every rate, RMS 0.46-0.72 dB.
// Where the derived curve is exactly 0.000 dB (mid-band) a sweep reads
// +0.13..+0.35 dB, which is the sweep's own noise -- the measurement scatters
// around the derivation, with no systematic offset.
//
// Clamped to [0, 90] dB to match AnanDroopCalibrator::computeCorrection()'s
// own cap, so a shipped default and a bench sweep occupy the same value
// domain. The unclamped curve reaches 209 dB at the outermost bin, which is
// meaningless -- and applyDroopCorrectionDb() adds without clamping, so an
// uncapped table would be a latent spike if applyEdgeFade()'s tail ever
// shrank.
//
// The gateware caveat is real: a future FPGA release could re-tune those
// filters. Defaults are applied regardless of the connected radio's reported
// gateware, deliberately -- a slightly-wrong correction beats no correction,
// and any operator who finds it wrong can run the in-app sweep, which then
// takes precedence for their radio. kDefaultsGatewareVersion records what
// these were derived against so a mismatch can be reasoned about rather than
// guessed at. Regenerate with tools/derive_droop_from_gateware.py.
inline constexpr int kDefaultsGatewareVersion = 27;

// The shipped table for one of the six valid DDC0 rates, or nullptr for any
// other rate. Every valid rate returns the SAME table -- see the note above.
[[nodiscard]] const DroopCorrectionTable* defaultDroopTableForRate(int rateKsps) noexcept;

// The six rates this ships defaults for, for callers that want to seed them
// all without hardcoding the list a second time.
[[nodiscard]] const std::array<int, 6>& defaultDroopRatesKsps() noexcept;

}  // namespace AetherSDR::anan
