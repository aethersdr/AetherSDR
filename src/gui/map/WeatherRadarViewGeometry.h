#pragma once

#include "WeatherRadarWorldWrap.h"

#include <QSize>
#include <limits>

namespace AetherSDR {

constexpr qint64 kMaximumWeatherRadarPixels = 4 * 1024 * 1024;

inline QSize weatherRadarLimitedSize(const QSize& size)
{
    const qint64 pixels = qint64(size.width()) * size.height();
    if (size.isEmpty() || pixels <= kMaximumWeatherRadarPixels) {
        return size;
    }
    const double scale = std::sqrt(double(kMaximumWeatherRadarPixels) / pixels);
    return QSize(std::max(1, int(std::floor(size.width() * scale))),
                 std::max(1, int(std::floor(size.height() * scale))));
}

struct WeatherRadarViewGeometry {
    // Always conventional EPSG:3857 (north positive), including cache entries.
    // Convert ONLY at the renderer boundary, never change a retained image's
    // bounds to match a newer camera. Pixels and bounds are one immutable unit.
    QRectF bounds;
    QSize size;

    bool covers(const WeatherRadarViewGeometry& view) const
    {
        if (bounds.isEmpty() || size.isEmpty() || view.bounds.isEmpty()
            || view.size.isEmpty() || !bounds.contains(view.bounds)) {
            return false;
        }
        // Small zoom/Retina rounding changes do not justify another movie.
        return bounds.width() / size.width()
                   <= 1.15 * view.bounds.width() / view.size.width()
            && bounds.height() / size.height()
                   <= 1.15 * view.bounds.height() / view.size.height();
    }
};

inline WeatherRadarViewGeometry weatherRadarPaddedView(
    const WeatherRadarViewGeometry& view, int maximumDimension,
    qint64 maximumPixels = std::numeric_limits<qint64>::max())
{
    if (view.bounds.isEmpty() || view.size.isEmpty()) {
        return {};
    }
    // Snap outward on a geographic grid, not to floating-point screen pixels.
    // A small pan then has an identical export URL and can reuse the disk/RAM
    // cache. Keep padding modest, and never grow beyond the provider/image cap.
    const double unitsX = view.bounds.width() / view.size.width();
    const double unitsY = view.bounds.height() / view.size.height();
    const auto axis = [maximumDimension](double first, double span, double units) {
        const double grid = std::pow(2.0, std::floor(std::log2(units * 128.0)));
        double low = std::max(-kRadarMercatorExtent, std::floor((first - span * .125) / grid) * grid);
        double high = std::min(kRadarMercatorExtent, std::ceil((first + span * 1.125) / grid) * grid);
        // At the texture cap reduce the MARGIN, not the data resolution. A
        // padded 2048px image must not replace an already-sharp 2048px view
        // with fewer pixels covering the storm, or trigger endless reloads.
        const double maximumSpan = std::max(span, maximumDimension * units);
        if (high - low > maximumSpan) {
            low = std::clamp(first + span / 2 - maximumSpan / 2,
                -kRadarMercatorExtent, kRadarMercatorExtent - maximumSpan);
            high = low + maximumSpan;
        }
        return QPair<double, double>(low, high);
    };
    if (!std::isfinite(unitsX) || !std::isfinite(unitsY) || maximumDimension <= 0) {
        return {};
    }
    const auto x = axis(view.bounds.left(), view.bounds.width(), unitsX);
    const auto y = axis(view.bounds.top(), view.bounds.height(), unitsY);
    const QRectF bounds(x.first, y.first, x.second - x.first, y.second - y.first);
    const QSize size(
        qRound(std::clamp(bounds.width() / unitsX, 1.0, double(maximumDimension))),
        qRound(std::clamp(bounds.height() / unitsY, 1.0, double(maximumDimension))));
    // Spend the pixel budget on the visible data first. Optional margins may
    // be dropped without softening the image or creating a cache/rebuffer loop.
    if (qint64(size.width()) * size.height() > maximumPixels) {
        return view;
    }
    return {bounds, size};
}

// Reorder only waiting work; do not abort useful in-flight frames whenever
// playback advances. This also makes missing/slow early frames independent of
// subsequent completions, rather than a five-frame head-of-line barrier.
inline int weatherRadarTakePriorityFrame(QVector<int>& queue, int displayed, int count)
{
    if (queue.isEmpty() || count <= 0) {
        return -1;
    }
    const int start = std::clamp(displayed, 0, count - 1);
    const auto best = std::min_element(queue.begin(), queue.end(), [start, count](int a, int b) {
        return (a - start + count) % count < (b - start + count) % count;
    });
    const int result = *best;
    queue.erase(best);
    return result;
}

} // namespace AetherSDR
