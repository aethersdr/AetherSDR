#pragma once

#include <QVector>

namespace AetherSDR {

// Pure-math helper: generates evenly-spaced sweep center frequencies (MHz)
// for an ATU pre-tune across [lowMhz, highMhz] using segmentKhz-wide
// segments.  Header-only so the unit test can include this directly
// without dragging in the dialog (#2648).
//
// Centers are evenly-spaced starting at lowMhz + seg/2.  Each segment of
// width segMhz fits inside the band when center + seg/2 <= highMhz; we
// keep adding centers while this holds.  If room remains at the top
// after the regular loop, an extra clamped center is appended at
// highMhz - seg/2 so the band end isn't left uncovered.  This matches
// the per-band counts in the IARU R1 reference table in issue #2624.
//
// Edge cases:
//   * segmentKhz <= 0 or highMhz <= lowMhz → empty result.
//   * (highMhz - lowMhz) < segMhz → empty (single segment cannot fit).
inline QVector<double> computeCenters(double lowMhz, double highMhz, int segmentKhz)
{
    QVector<double> out;
    if (segmentKhz <= 0 || highMhz <= lowMhz) return out;
    const double segMhz = segmentKhz / 1000.0;
    const double half = segMhz / 2.0;
    const double firstCenter = lowMhz + half;
    const double lastAllowedCenter = highMhz - half;
    if (lastAllowedCenter < firstCenter - 1e-9) return out;

    constexpr double kEps = 1e-9;
    for (double f = firstCenter; f <= lastAllowedCenter + kEps; f += segMhz) {
        out.append(f);
    }
    if (!out.isEmpty() && out.last() < lastAllowedCenter - kEps) {
        out.append(lastAllowedCenter);
    }
    return out;
}

} // namespace AetherSDR
