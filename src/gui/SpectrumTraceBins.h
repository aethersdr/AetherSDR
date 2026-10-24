#pragma once

// The per-frame trace a SpectrumWidget draws from, for the bridge's
// `get tracedebug <pan> bins` (#5806). Kept free of QtWidgets so it can be
// tested without a widget.

#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

#include <cmath>

namespace AetherSDR::SpectrumTraceBins {

// bins: the trace before display smoothing, in the order the widget draws them
// across centerMhz ± bandwidthMhz/2. A non-finite bin is reported as null.
inline QVariantMap snapshot(const QVector<float>& bins, double centerMhz,
                            double bandwidthMhz, quint64 fftFrameSequence)
{
    const double spanHz = bandwidthMhz * 1e6;
    QVariantList values;
    values.reserve(bins.size());
    int finiteCount = 0;
    for (float v : bins) {
        if (std::isfinite(v)) {
            values.append(static_cast<double>(v));
            ++finiteCount;
        } else {
            values.append(QVariant());
        }
    }
    QVariantMap m;
    m[QStringLiteral("count")] = bins.size();
    m[QStringLiteral("finiteCount")] = finiteCount;
    m[QStringLiteral("centerHz")] = centerMhz * 1e6;
    m[QStringLiteral("spanHz")] = spanHz;
    m[QStringLiteral("lowHz")] = centerMhz * 1e6 - spanHz / 2.0;
    m[QStringLiteral("binHz")] = bins.isEmpty() ? 0.0 : spanHz / bins.size();
    m[QStringLiteral("fftFrameSequence")] = QVariant::fromValue(fftFrameSequence);
    m[QStringLiteral("bins")] = values;
    return m;
}

}  // namespace AetherSDR::SpectrumTraceBins
