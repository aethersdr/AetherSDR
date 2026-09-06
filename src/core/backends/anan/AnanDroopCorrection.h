#pragma once

#include <array>
#include <cmath>
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
// dB correction, measured empirically per DDC0 rate by the in-app
// AnanDroopCalibrator sweep (src/core/backends/anan/AnanDroopCalibrator.h),
// to the actual FFT magnitude before display. AnanRxDsp holds the live
// table set (loaded from per-radio settings at connect, or produced by a
// fresh sweep); this header only owns the data shape and the pure apply
// math, not table selection or storage.

inline constexpr int kDroopCorrectionFftSize = 1024;

using DroopCorrectionTable = std::array<float, kDroopCorrectionFftSize>;

// Safe no-op fallback for an unrecognized/uncalibrated rate -- additive
// zero, not a guess.
extern const DroopCorrectionTable& kDroopCorrectionZero;

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

// Cosmetic fade for the outermost `tailFraction` of bins on each side,
// applied AFTER applyDroopCorrectionDb() -- for the true edge of the span,
// not for the recoverable bulk of it.
//
// The measured droop at the true edge is deep enough, and noisy enough bin
// to bin, that no per-bin dB correction produces a clean result: raising
// AnanDroopCalibrator's capDb from 70 to 90 dB left those bins unchanged or
// worse from one calibration sweep to the next, because the limiting
// factor there is measurement noise near the ADC's effective floor, not
// correction headroom. Chasing more gain just amplifies that noise instead
// of recovering real signal.
//
// This function does not try. It overwrites the tail zone with a
// deterministic raised-cosine fade from the corrected value at the tail
// boundary down to (boundary - fadeDb), replacing whatever noisy value the
// real droop + correction produced there -- so the display always shows a
// smooth, repeatable roll-off at the true edge instead of an unpredictable
// one that sometimes drops below the panadapter's black level and reads as
// a broken/glitchy dark band. This is the same judgment call WDSP's own
// Display/Analyzer API makes: SetAnalyzer's `clp` parameter exists to clip
// a decimation filter's roll-off rather than display it ("It is generally
// not desirable to display the roll-off area... A primary use of this
// capability is to clip off those bins", WDSP_Guide Rev 2.00 Section 7.2).
// We fade instead of literally clipping bins because changing bin
// count/reported bandwidth already broke zoom-out once -- see
// AnanBackend::emitPanState()'s own comment.
inline void applyEdgeFade(std::vector<float>& binsDbfs,
                           float tailFraction = 0.03f,
                           float fadeDb = 12.0f) noexcept
{
    const auto n = binsDbfs.size();
    const auto tailBins = static_cast<std::size_t>(static_cast<float>(n) * tailFraction);
    // Require at least one untouched bin strictly between the two tail
    // zones -- otherwise they'd overlap (or abut with no gap), and
    // rightBoundary below could read a bin the left loop already
    // overwrote.
    if (tailBins < 2 || n <= tailBins * 2)
        return;

    constexpr float kPi = 3.14159265358979323846f;

    // Left edge: bin 0 is the true edge, bin tailBins is the boundary this
    // fade blends FROM (left untouched). u runs 1 (true edge) -> 0
    // (boundary), so the raised-cosine window is 0 right at the boundary
    // (perfect continuity with the untouched region) and 1 at the true
    // edge (full fadeDb applied).
    const float leftBoundary = binsDbfs[tailBins];
    for (std::size_t k = 0; k < tailBins; ++k) {
        const float u = static_cast<float>(tailBins - k) / static_cast<float>(tailBins);
        const float window = 0.5f * (1.0f - std::cos(u * kPi));
        binsDbfs[k] = leftBoundary - fadeDb * window;
    }

    // Right edge: mirror image, boundary at n-1-tailBins.
    const float rightBoundary = binsDbfs[n - 1 - tailBins];
    for (std::size_t k = 0; k < tailBins; ++k) {
        const std::size_t idx = n - 1 - k;
        const float u = static_cast<float>(tailBins - k) / static_cast<float>(tailBins);
        const float window = 0.5f * (1.0f - std::cos(u * kPi));
        binsDbfs[idx] = rightBoundary - fadeDb * window;
    }
}

}  // namespace AetherSDR::anan
