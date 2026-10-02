#pragma once

#include "core/backends/anan/AnanDroopCorrection.h"

namespace AetherSDR::anan {

// Compiled-in droop-correction defaults for the ANAN-G2. connectRadio() seeds
// these, then overlays whatever the in-app sweep persisted for THIS radio.
// Derived from the Saturn gateware, not measured, so one curve fits every unit:
// ADC 122.88 MSPS -> CIC (6 stages, dd 1, R = 10..320) -> FIR (1024 taps, /8,
// 18-bit coefficients) -> 122.88e6 / (R * 8), i.e. 48..1536 ksps. The FIR
// transition band is the droop (flat +/-0.2 dB to 8% in from each edge, -6 dB at
// 5%, -15 at 4%, -30 at 3%, -54 at 2%); the CIC term varies 0.003 dB across rates,
// so defaultDroopTableForRate() returns the same table for every valid rate and
// only rejects invalid ones. Hardware check (G2, gateware 27): mean bias <= 0.13
// dB, RMS 0.46-0.72 dB at every rate. Clamped to [0, 90] dB, the same cap as
// AnanDroopCalibrator::computeCorrection() (the raw curve reaches 209 dB).
// Full derivation: tools/derive_droop_from_gateware.py.

// Trustworthy only from table point 14 inward: the gateware quantizes FIR taps
// to 18 bits (DDC_Block_fir_compiler_0_0.xci), which floors the real response
// near -125 dB, while this unquantized derivation runs to -209 dB (+94.8 dB error
// at bin 0, -2.2 at 11, <1 dB from 14 in, exact from 25). Points 0-13 are ~1.4%
// of the span, hidden only because applyEdgeFade() overwrites 3% (tailFraction
// 0.03, in another file). If that tail drops below ~0.014, re-derive against
// quantized taps. Applied regardless of reported gateware: slightly wrong beats
// uncorrected, and a sweep overrides it. Regenerate with
// tools/derive_droop_from_gateware.py.
inline constexpr int kDefaultsGatewareVersion = 27;

// The shipped table for one of the six valid DDC0 rates, or nullptr for any
// other rate. Every valid rate returns the SAME table -- see the note above.
[[nodiscard]] const DroopCorrectionTable* defaultDroopTableForRate(int rateKsps) noexcept;

// The six rates this ships defaults for, for callers that want to seed them
// all without hardcoding the list a second time.
[[nodiscard]] const std::array<int, 6>& defaultDroopRatesKsps() noexcept;

}  // namespace AetherSDR::anan
