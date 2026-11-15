#pragma once

#include <QVector>

namespace AetherSDR {

// Evenly spaced ATU pre-tune sweep centres (MHz) across [lowMhz, highMhz] in
// segmentKhz segments, header-only for the unit test (#2648). Centres start at
// lowMhz + seg/2 and continue while center + seg/2 <= highMhz; if room remains,
// a clamped centre at highMhz - seg/2 covers the band top. Matches the IARU R1
// per-band counts in #2624. Empty if segmentKhz <= 0, highMhz <= lowMhz, or the
// span is narrower than one segment.
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
