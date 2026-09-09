#pragma once

#include <QRectF>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

inline constexpr double kRadarMercatorExtent = 20037508.342789244;
inline constexpr double kRadarWorldWidth = 2.0 * kRadarMercatorExtent;

// Export one canonical copy, never a many-world bbox which ArcGIS cannot
// repeat. Ordinary storm close-ups keep their cropped native resolution even
// when the camera is several worlds east/west of the original map.
inline QRectF weatherRadarCanonicalPlaybackBounds(const QRectF& cameraBounds)
{
    const QRectF view = cameraBounds.normalized();
    if (!view.isValid() || !std::isfinite(view.left())
        || !std::isfinite(view.right()) || !std::isfinite(view.top())
        || !std::isfinite(view.bottom())) {
        return {};
    }
    const double top = std::max(view.top(), -kRadarMercatorExtent);
    const double bottom = std::min(view.bottom(), kRadarMercatorExtent);
    if (bottom <= top) {
        return {};
    }
    const double left = view.left() - kRadarWorldWidth
        * std::floor((view.left() + kRadarMercatorExtent) / kRadarWorldWidth);
    if (view.width() >= kRadarWorldWidth
        || left + view.width() > kRadarMercatorExtent) {
        // A dateline-spanning or multi-world view needs both ends of the
        // canonical image. Reuse it at integer world offsets when drawing.
        return QRectF(-kRadarMercatorExtent, top, kRadarWorldWidth, bottom - top);
    }
    return QRectF(left, top, view.width(), bottom - top);
}

inline QVector<double> weatherRadarVisibleWorldOffsets(
    const QRectF& sourceBounds, const QRectF& cameraBounds)
{
    const QRectF view = cameraBounds.normalized();
    if (sourceBounds.isEmpty() || view.isEmpty()
        || !std::isfinite(view.left()) || !std::isfinite(view.right())
        || sourceBounds.bottom() <= view.top() || sourceBounds.top() >= view.bottom()) {
        return {};
    }
    const double first = std::floor((view.left() - sourceBounds.right()) / kRadarWorldWidth) + 1;
    const double last = std::ceil((view.right() - sourceBounds.left()) / kRadarWorldWidth) - 1;
    QVector<double> offsets;
    // Match the basemap's bounded world-copy policy under extreme aspect ratios.
    const double center = std::round((view.center().x() - sourceBounds.center().x()) / kRadarWorldWidth);
    for (double copy = std::max(first, center - 4); copy <= std::min(last, center + 4); ++copy) {
        offsets.append(copy * kRadarWorldWidth);
    }
    return offsets;
}

} // namespace AetherSDR
