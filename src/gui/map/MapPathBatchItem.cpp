#include "MapPathBatchItem.h"
#include "MapPathGeometry.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QPainter>
#include <QSize>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr int kSegments = 32;
constexpr int kMaxWorldCopies = 4;
constexpr int kCacheRebuildDelayMs = 250;
constexpr int kOverviewRefreshMs = 30000;
constexpr int kMaxCacheDimension = 3072;
constexpr int kOverviewCacheDimension = 1024;
constexpr double kCacheMargin = 0.5;

QGV::GeoPos slerp(double lat1, double lon1, double lat2, double lon2,
                  double fraction)
{
    const double phi1 = qDegreesToRadians(lat1);
    const double lambda1 = qDegreesToRadians(lon1);
    const double phi2 = qDegreesToRadians(lat2);
    const double lambda2 = qDegreesToRadians(lon2);
    const double distance = 2.0 * std::asin(std::sqrt(
        std::pow(std::sin((phi1 - phi2) / 2.0), 2)
        + std::cos(phi1) * std::cos(phi2)
              * std::pow(std::sin((lambda1 - lambda2) / 2.0), 2)));
    if (distance < 1e-9) {
        return {lat1, lon1};
    }
    const double a = std::sin((1.0 - fraction) * distance)
                   / std::sin(distance);
    const double b = std::sin(fraction * distance) / std::sin(distance);
    const double x = a * std::cos(phi1) * std::cos(lambda1)
                   + b * std::cos(phi2) * std::cos(lambda2);
    const double y = a * std::cos(phi1) * std::sin(lambda1)
                   + b * std::cos(phi2) * std::sin(lambda2);
    const double z = a * std::sin(phi1) + b * std::sin(phi2);
    return {qRadiansToDegrees(std::atan2(z, std::sqrt(x * x + y * y))),
            qRadiansToDegrees(std::atan2(y, x))};
}

QPointF geoToWebMercator(double latitude, double longitude,
                         const QRectF& worldRect)
{
    constexpr double kMaxLatitude = 85.0;
    const double originShift = worldRect.width() / 2.0;
    const double lat = std::clamp(latitude, -kMaxLatitude, kMaxLatitude);
    const double x = longitude * originShift / 180.0;
    const double preY = -std::log(std::tan(
        (90.0 + lat) * M_PI / 360.0)) / (M_PI / 180.0);
    const double y = preY * originShift / 180.0;
    return {x, y};
}

void disableRedundantDeviceCache(QGVDrawItem* drawItem, QGVMap* geoMap)
{
    for (QGraphicsItem* item : geoMap->geoView()->scene()->items()) {
        if (QGVMapQGItem::geoObjectFromQGItem(item) == drawItem) {
            item->setCacheMode(QGraphicsItem::NoCache);
            return;
        }
    }
}
} // namespace

MapPathBatchItem::MapPathBatchItem(const QVector<MapView::Marker>& markers,
                                   bool hasHome, double homeLat, double homeLon)
    : m_markers(markers)
    , m_directPaint(markers.size() == 1)
    , m_hasHome(hasHome)
    , m_homeLat(homeLat)
    , m_homeLon(homeLon)
{
    setSelectable(false);
    setZValue(-1);
    m_cacheTimer.setSingleShot(true);
    m_cacheTimer.setInterval(kCacheRebuildDelayMs);
    connect(&m_cacheTimer, &QTimer::timeout,
            this, &MapPathBatchItem::rebuildCache);
    m_overviewRefreshTimer.setSingleShot(true);
    m_overviewRefreshTimer.setInterval(kOverviewRefreshMs);
    connect(&m_overviewRefreshTimer, &QTimer::timeout,
            this, &MapPathBatchItem::rebuildOverviewCache);
}

MapPathBatchItem::~MapPathBatchItem()
{
    if (m_projectionCancelled) {
        m_projectionCancelled->store(true, std::memory_order_relaxed);
    }
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
}

void MapPathBatchItem::setMarkers(
    const QVector<MapView::Marker>& markers,
    bool hasHome, double homeLat, double homeLon)
{
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
    m_markers = markers;
    m_directPaint = markers.size() == 1;
    m_hasHome = hasHome;
    m_homeLat = homeLat;
    m_homeLon = homeLon;
    if (getMap() == nullptr) {
        return;
    }
    rebuildProjectedPaths();
}

void MapPathBatchItem::rebuildProjectedPaths()
{
    if (m_worldRect.isEmpty() || !isVisible()) {
        return;
    }
    if (m_projectionCancelled) {
        m_projectionCancelled->store(true, std::memory_order_relaxed);
    }
    const quint64 generation = ++m_projectionGeneration;
    m_projectionCancelled = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelled = m_projectionCancelled;

    if (m_directPaint) {
        m_paths = projectPaths(m_markers, m_hasHome, m_homeLat, m_homeLon,
                               m_worldRect, cancelled);
        refresh();
        return;
    }

    auto* watcher = new QFutureWatcher<QVector<Path>>(this);
    connect(watcher, &QFutureWatcher<QVector<Path>>::finished, this,
            [this, watcher, generation] {
                QVector<Path> paths = watcher->result();
                watcher->deleteLater();
                if (generation != m_projectionGeneration || !isVisible()) {
                    return;
                }
                m_paths = std::move(paths);
                // Retain the old images until their replacements complete.
                // A whole-world overview is only needed immediately for the
                // first data set; subsequent MQTT batches refresh it slowly.
                if (m_overviewCache.isNull()) {
                    rebuildOverviewCache();
                } else if (!m_overviewRefreshTimer.isActive()) {
                    m_overviewRefreshTimer.start();
                }
                rebuildCache();
                refresh();
            });
    watcher->setFuture(QtConcurrent::run(
        &MapPathBatchItem::projectPaths, m_markers, m_hasHome,
        m_homeLat, m_homeLon, m_worldRect, cancelled));
}

QVector<MapPathBatchItem::Path> MapPathBatchItem::projectPaths(
    QVector<MapView::Marker> markers, bool hasHome,
    double homeLat, double homeLon, QRectF worldRect,
    std::shared_ptr<std::atomic_bool> cancelled)
{
    const double worldWidth = worldRect.width();
    QVector<Path> paths;
    paths.reserve(markers.size());
    for (const MapView::Marker& marker : std::as_const(markers)) {
        if (cancelled->load(std::memory_order_relaxed)) {
            return {};
        }
        if (!marker.pathEnabled
            || (!marker.hasPathOrigin && !hasHome)) {
            continue;
        }
        const double fromLat = marker.hasPathOrigin
            ? marker.pathFromLat : homeLat;
        const double fromLon = marker.hasPathOrigin
            ? marker.pathFromLon : homeLon;
        QPainterPath projectedPath;
        QPointF previous;
        for (int segment = 0; segment <= kSegments; ++segment) {
            const double fraction = static_cast<double>(segment) / kSegments;
            const QGV::GeoPos geo = slerp(
                fromLat, fromLon, marker.lat, marker.lon, fraction);
            QPointF point = geoToWebMercator(
                geo.latitude(), geo.longitude(), worldRect);
            if (segment == 0) {
                projectedPath.moveTo(point);
            } else {
                point.setX(MapPathGeometry::unwrapX(
                    point.x(), previous.x(), worldWidth));
                projectedPath.lineTo(point);
            }
            previous = point;
        }
        Path path;
        path.projected = projectedPath;
        path.bounds = projectedPath.boundingRect();
        path.color = marker.color;
        paths.append(path);
    }
    return paths;
}

void MapPathBatchItem::setDisplayVisible(bool visible)
{
    if (isVisible() == visible) {
        return;
    }
    setVisible(visible);
    if (m_directPaint) {
        repaint();
        return;
    }
    if (!visible) {
        m_cacheTimer.stop();
        ++m_cacheGeneration;
        ++m_overviewGeneration;
        ++m_projectionGeneration;
        if (m_cacheCancelled) {
            m_cacheCancelled->store(true, std::memory_order_relaxed);
        }
        if (m_overviewCancelled) {
            m_overviewCancelled->store(true, std::memory_order_relaxed);
        }
        if (m_projectionCancelled) {
            m_projectionCancelled->store(true, std::memory_order_relaxed);
        }
        return;
    }
    // Reuse any completed cache immediately while refreshing geometry for
    // marker updates or camera changes made while Paths was hidden.
    rebuildProjectedPaths();
    repaint();
}

void MapPathBatchItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    // This item already owns a bounded raster cache. QGeoView's default
    // device-coordinate cache spans the multi-world item and is invalidated
    // by every smooth trackpad scale step, making it both redundant and
    // potentially enormous at high zoom.
    disableRedundantDeviceCache(this, geoMap);
    m_worldRect = geoMap->getProjection()->boundaryProjRect();
    rebuildProjectedPaths();
    resetBoundary();
    refresh();
}

void MapPathBatchItem::onCamera(const QGVCameraState& oldState,
                                const QGVCameraState& newState)
{
    QGVDrawItem::onCamera(oldState, newState);
    if (!isVisible() || m_directPaint) {
        return;
    }
    if (m_cache.isNull()) {
        rebuildCache();
        return;
    }
    const bool scaleChanged = m_cacheScale <= 0.0
        || std::abs(newState.scale() / m_cacheScale - 1.0) > 0.02;
    const QRectF safeRect = m_cacheRect.adjusted(
        newState.projRect().width() * 0.25,
        newState.projRect().height() * 0.25,
        -newState.projRect().width() * 0.25,
        -newState.projRect().height() * 0.25);
    if (scaleChanged || !safeRect.contains(newState.projRect())) {
        scheduleCacheRebuild();
    }
}

void MapPathBatchItem::scheduleCacheRebuild()
{
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    ++m_cacheGeneration;
    m_cacheTimer.start();
}

void MapPathBatchItem::rebuildCache()
{
    if (getMap() == nullptr || m_worldRect.isEmpty()) {
        return;
    }
    const QGVCameraState& camera = getMap()->getCamera();
    const QRectF visible = camera.projRect();
    if (visible.isEmpty() || camera.scale() <= 0.0) {
        return;
    }
    QRectF cacheRect = visible.adjusted(
        -visible.width() * kCacheMargin,
        -visible.height() * kCacheMargin,
        visible.width() * kCacheMargin,
        visible.height() * kCacheMargin);
    cacheRect.setTop(qMax(cacheRect.top(), m_worldRect.top()));
    cacheRect.setBottom(qMin(cacheRect.bottom(), m_worldRect.bottom()));
    const double cacheScale = qMin(camera.scale(), qMin(
        static_cast<double>(kMaxCacheDimension) / cacheRect.width(),
        static_cast<double>(kMaxCacheDimension) / cacheRect.height()));
    const quint64 generation = ++m_cacheGeneration;
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    m_cacheCancelled = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelled = m_cacheCancelled;
    auto* watcher = new QFutureWatcher<CacheResult>(this);
    connect(watcher, &QFutureWatcher<CacheResult>::finished, this,
            [this, watcher, generation] {
                const CacheResult result = watcher->result();
                watcher->deleteLater();
                if (generation != m_cacheGeneration || result.image.isNull()) {
                    return;
                }
                m_cacheRect = result.rect;
                m_cacheScale = result.scale;
                m_cache = result.image;
                repaint();
            });
    watcher->setFuture(QtConcurrent::run(
        &MapPathBatchItem::renderCache, m_paths, m_worldRect,
        cacheRect, cacheScale, cancelled));
}

void MapPathBatchItem::rebuildOverviewCache()
{
    if (m_worldRect.isEmpty() || m_paths.isEmpty()) {
        return;
    }
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
    const double scale = qMin(
        static_cast<double>(kOverviewCacheDimension) / m_worldRect.width(),
        static_cast<double>(kOverviewCacheDimension) / m_worldRect.height());
    const quint64 generation = ++m_overviewGeneration;
    m_overviewCancelled = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelled = m_overviewCancelled;
    auto* watcher = new QFutureWatcher<CacheResult>(this);
    connect(watcher, &QFutureWatcher<CacheResult>::finished, this,
            [this, watcher, generation] {
                const CacheResult result = watcher->result();
                watcher->deleteLater();
                if (generation != m_overviewGeneration
                    || result.image.isNull()) {
                    return;
                }
                m_overviewCache = result.image;
                repaint();
            });
    watcher->setFuture(QtConcurrent::run(
        &MapPathBatchItem::renderCache, m_paths, m_worldRect,
        m_worldRect, scale, cancelled));
}

MapPathBatchItem::CacheResult MapPathBatchItem::renderCache(
    QVector<Path> paths, QRectF worldRect, QRectF cacheRect, double cacheScale,
    std::shared_ptr<std::atomic_bool> cancelled)
{
    if (cancelled->load(std::memory_order_relaxed)) {
        return {};
    }
    const QSize imageSize(
        qMax(1, qCeil(cacheRect.width() * cacheScale)),
        qMax(1, qCeil(cacheRect.height() * cacheScale)));
    QImage cache(imageSize, QImage::Format_ARGB32_Premultiplied);
    cache.fill(Qt::transparent);

    QPainter cachePainter(&cache);
    cachePainter.setRenderHint(QPainter::Antialiasing);
    cachePainter.setBrush(Qt::NoBrush);
    cachePainter.setWorldTransform(QTransform(
        cacheScale, 0.0, 0.0, cacheScale,
        -cacheRect.left() * cacheScale,
        -cacheRect.top() * cacheScale));
    const double worldWidth = worldRect.width();
    QRgb activeColor = 0;
    for (const Path& path : std::as_const(paths)) {
        if (cancelled->load(std::memory_order_relaxed)) {
            cachePainter.end();
            return {};
        }
        const int firstCopy = static_cast<int>(std::floor(
            (cacheRect.left() - path.bounds.right()) / worldWidth));
        const int lastCopy = static_cast<int>(std::ceil(
            (cacheRect.right() - path.bounds.left()) / worldWidth));
        const QRgb pathColor = path.color.rgba();
        QColor color = path.color;
        color.setAlpha(120);
        if (pathColor != activeColor) {
            QPen pen(color, 2.5);
            pen.setCosmetic(true);
            cachePainter.setPen(pen);
            activeColor = pathColor;
        }
        for (int copy = firstCopy; copy <= lastCopy; ++copy) {
            const QRectF bounds = path.bounds.translated(copy * worldWidth, 0.0);
            if (!bounds.intersects(cacheRect)) {
                continue;
            }
            cachePainter.save();
            cachePainter.translate(copy * worldWidth, 0.0);
            cachePainter.drawPath(path.projected);
            cachePainter.restore();
        }
    }
    cachePainter.end();
    return {cacheRect, cache, cacheScale};
}

QPainterPath MapPathBatchItem::projShape() const
{
    QPainterPath shape;
    for (int copy = -kMaxWorldCopies; copy <= kMaxWorldCopies; ++copy) {
        shape.addRect(m_worldRect.translated(copy * m_worldRect.width(), 0.0));
    }
    return shape;
}

void MapPathBatchItem::projPaint(QPainter* painter)
{
    if (m_directPaint) {
        if (m_paths.isEmpty() || getMap() == nullptr) {
            return;
        }
        painter->setRenderHint(QPainter::Antialiasing);
        QColor color = m_paths.first().color;
        color.setAlpha(170);
        QPen pen(color, 3.0);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        const QRectF visible = getMap()->getCamera().projRect();
        const double worldWidth = m_worldRect.width();
        const Path& path = m_paths.first();
        const int firstCopy = static_cast<int>(std::floor(
            (visible.left() - path.bounds.right()) / worldWidth));
        const int lastCopy = static_cast<int>(std::ceil(
            (visible.right() - path.bounds.left()) / worldWidth));
        for (int copy = firstCopy; copy <= lastCopy; ++copy) {
            painter->save();
            painter->translate(copy * worldWidth, 0.0);
            painter->drawPath(path.projected);
            painter->restore();
        }
        return;
    }
    if ((m_cache.isNull() || m_cacheRect.isEmpty())
        && m_overviewCache.isNull()) {
        return;
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF visible = getMap() != nullptr
        ? getMap()->getCamera().projRect() : QRectF();
    if (!m_overviewCache.isNull() && !visible.isEmpty()
        && (m_cache.isNull() || !m_cacheRect.contains(visible))) {
        const double worldWidth = m_worldRect.width();
        const int firstCopy = static_cast<int>(std::floor(
            (visible.left() - m_worldRect.right()) / worldWidth));
        const int lastCopy = static_cast<int>(std::ceil(
            (visible.right() - m_worldRect.left()) / worldWidth));
        for (int copy = firstCopy; copy <= lastCopy; ++copy) {
            painter->drawImage(
                m_worldRect.translated(copy * worldWidth, 0.0),
                m_overviewCache);
        }
    }
    if (!m_cache.isNull() && !m_cacheRect.isEmpty()) {
        painter->drawImage(m_cacheRect, m_cache);
    }
}

} // namespace AetherSDR
