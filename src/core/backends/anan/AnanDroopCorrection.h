#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace AetherSDR::anan {

// The Saturn FPGA's DDC0 decimation chain (two cascaded CIC decimators plus
// a halfband FIR -- see reference/saturn/New_protocol_FPGA_Block_diagrams.pdf,
// "Receiver(3)") imposes a REAL sin(x)/x amplitude droop near the edges of
// the displayed span, baked into the raw IQ samples themselves. This is not
// a rendering artifact and not fixable by touching bin count or reported
// bandwidth -- an earlier attempt at exactly that broke zoom-out (see
// AnanBackend::emitPanState()'s own comment). This header applies a per-bin
// dB correction, measured empirically per DDC0 rate (tools/
// anan_droop_calibration.py), to the actual FFT magnitude before display.

inline constexpr int kDroopCorrectionFftSize = 1024;

using DroopCorrectionTable = std::array<float, kDroopCorrectionFftSize>;

// Safe no-op fallback for an unrecognized rate -- additive zero, not a guess.
extern const DroopCorrectionTable& kDroopCorrectionZero;

// Selects the measured table for a DDC0 rate in ksps (one of the six ANAN-G2
// values: 48/96/192/384/768/1536). Returns kDroopCorrectionZero for any
// other rate.
[[nodiscard]] const DroopCorrectionTable& droopCorrectionTableForRateKsps(int ddc0RateKsps) noexcept;

// Adds the per-bin dB correction into binsDbfs in place. Pure, no I/O, no
// AnanRxDsp state -- unit-testable standalone. A size mismatch leaves
// binsDbfs byte-for-byte unchanged rather than truncating or asserting: a
// table generated for a different fftSize must never silently misalign bin
// k against the wrong correction.
inline void applyDroopCorrectionDb(std::vector<float>& binsDbfs,
                                    const DroopCorrectionTable& table) noexcept
{
    if (binsDbfs.size() != table.size())
        return;
    for (std::size_t i = 0; i < binsDbfs.size(); ++i)
        binsDbfs[i] += table[i];
}

}  // namespace AetherSDR::anan
