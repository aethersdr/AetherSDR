#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace AetherSDR::anan {

// The Saturn FPGA's DDC0 decimation chain imposes a REAL amplitude roll-off
// near the edges of the displayed span, baked into the raw IQ samples
// themselves. The chain is a 6-stage CIC decimator (differential delay 1,
// R = 10..320) followed by a 1024-tap decimate-by-8 anti-alias FIR, and
// across the DDC OUTPUT band the roll-off is almost entirely that FIR's
// transition band: the CIC term contributes at most 0.34 dB and varies by
// 0.003 dB between the fastest and slowest rate. It is NOT the CIC's
// sin(x)/x droop, which earlier revisions of this comment claimed -- that
// mattered, because a sin(x)/x story implies a per-rate curve while the real
// cause gives one curve for all six rates (see AnanDroopDefaults.h).
//
// This is not a rendering artifact and not fixable by touching bin count or
// reported bandwidth -- an earlier attempt at exactly that broke zoom-out
// (see AnanBackend::emitPanState()'s own comment). This header applies a
// per-bin dB correction to the actual FFT magnitude before display. Two
// sources feed it: the derived defaults seeded at connect
// (AnanDroopDefaults.h) and, on top, whatever the in-app AnanDroopCalibrator
// sweep (AnanDroopCalibrator.h) measured for that specific radio. AnanRxDsp
// holds the live table set; this header only owns the data shape and the
// pure apply math, not table selection or storage.

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

// Value of a kDroopCorrectionFftSize-point curve at output point `i` of
// `points`, by linear interpolation. Both grids run edge to edge over the
// same span with their end points on the span's edges -- the analyzer's
// point grid is laid out that way for any count -- so point i sits at
// fraction i / (points - 1) of the span on either grid.
inline float droopCurveAt(const DroopCorrectionTable& table, std::size_t i,
                          std::size_t points) noexcept
{
    if (points < 2)
        return table[kDroopCorrectionFftSize / 2];
    const double x = static_cast<double>(i) * (kDroopCorrectionFftSize - 1)
        / static_cast<double>(points - 1);
    const auto j = std::min(static_cast<std::size_t>(x),
                            static_cast<std::size_t>(kDroopCorrectionFftSize - 2));
    const float frac = static_cast<float>(x - static_cast<double>(j));
    return table[j] + frac * (table[j + 1] - table[j]);
}

// applyDroopCorrectionDb() for a point count that follows the panel width.
// The tables stay at kDroopCorrectionFftSize points so stored calibrations
// and the derived defaults need no re-sweep; the correction is a smooth
// gain-versus-frequency curve, so reading it between its points is sound.
// At exactly kDroopCorrectionFftSize points this is applyDroopCorrectionDb().
inline void applyDroopCorrectionDbResampled(std::vector<float>& pointsDb,
                                            const DroopCorrectionTable& table) noexcept
{
    const std::size_t n = pointsDb.size();
    if (n == table.size()) {
        applyDroopCorrectionDb(pointsDb, table);
        return;
    }
    if (n < 2)
        return;
    for (std::size_t i = 0; i < n; ++i)
        pointsDb[i] += droopCurveAt(table, i, n);
}

// The reverse direction, for the calibrator: a frame of any point count read
// onto the kDroopCorrectionFftSize grid the tables are stored on. Each table
// point takes the frame's value at the same fraction of the span, by linear
// interpolation. Returns false, leaving `out` untouched, for a frame with
// fewer than two points.
inline bool resampleToDroopGrid(const std::vector<float>& pointsDb,
                                DroopCorrectionTable& out) noexcept
{
    const std::size_t n = pointsDb.size();
    if (n < 2)
        return false;
    for (std::size_t k = 0; k < kDroopCorrectionFftSize; ++k) {
        const double x = static_cast<double>(k) * static_cast<double>(n - 1)
            / (kDroopCorrectionFftSize - 1);
        const auto j = std::min(static_cast<std::size_t>(x), n - 2);
        const float frac = static_cast<float>(x - static_cast<double>(j));
        out[k] = pointsDb[j] + frac * (pointsDb[j + 1] - pointsDb[j]);
    }
    return true;
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
