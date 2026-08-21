#include "MapMarkerBatchItem.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QPainter>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>

namespace AetherSDR {

namespace {
constexpr double kDotRadius = 5.0;
constexpr double kMonitorRadius = 7.0;
constexpr double kHitRadius = 9.0;
constexpr double kLabelGap = 3.0;
constexpr int kMaxWorldCopies = 4;
constexpr int kCacheRebuildDelayMs = 250;
constexpr int kOverviewRefreshMs = 30000;
constexpr int kMaxCacheDimension = 3072;
constexpr int kOverviewCacheDimension = 2048;
constexpr double kCacheMargin = 0.5;

QFont markerFont()
{
    QFont font;
    font.setPointSizeF(9.0);
    font.setBold(true);
    return font;
}

double radiusFor(const MapView::Marker& marker)
{
    return marker.isMonitor ? kMonitorRadius : kDotRadius;
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

MapMarkerBatchItem::MapMarkerBatchItem(
    const QVector<MapView::Marker>& markers,
    const QColor& labelHalo, const QColor& labelText)
    : m_markers(markers)
    , m_labelHalo(labelHalo)
    , m_labelText(labelText)
{
    setSelectable(false);
    m_cacheTimer.setSingleShot(true);
    m_cacheTimer.setInterval(kCacheRebuildDelayMs);
    connect(&m_cacheTimer, &QTimer::timeout,
            this, &MapMarkerBatchItem::rebuildCache);
    m_overviewRefreshTimer.setSingleShot(true);
    m_overviewRefreshTimer.setInterval(kOverviewRefreshMs);
    connect(&m_overviewRefreshTimer, &QTimer::timeout,
            this, &MapMarkerBatchItem::rebuildOverviewCache);
}

MapMarkerBatchItem::~MapMarkerBatchItem()
{
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
}

void MapMarkerBatchItem::setMarkers(
    const QVector<MapView::Marker>& markers)
{
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
    m_markers = markers;
    if (getMap() == nullptr) {
        return;
    }
    projectMarkers(getMap()->getProjection());
    // Keep the existing detail image visible while its replacement renders.
    // The whole-world overview is only a brief camera-gesture fallback; a
    // multi-megapixel rebuild for every MQTT report wastes CPU and exposes its
    // different effective pixel scale between detail frames.
    rebuildCache();
    if (!m_overviewRefreshTimer.isActive()) {
        m_overviewRefreshTimer.start();
    }
    refresh();
}

void MapMarkerBatchItem::projectMarkers(const QGVProjection* projection)
{
    m_projected.clear();
    m_projected.reserve(m_markers.size());
    for (const MapView::Marker& marker : std::as_const(m_markers)) {
        m_projected.append(projection->geoToProj(
            QGV::GeoPos(marker.lat, marker.lon)));
    }
}

void MapMarkerBatchItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    disableRedundantDeviceCache(this, geoMap);
    const QGVProjection* projection = geoMap->getProjection();
    m_worldRect = projection->boundaryProjRect();
    projectMarkers(projection);
    // The current-camera detail is the first useful frame. Rendering the
    // whole-world gesture fallback at the same time makes two large images
    // compete for the thread pool and delays initial marker visibility.
    // Build the overview after this first detail image completes; until then
    // the detail cache remains visible.
    rebuildCache();
    resetBoundary();
    refresh();
}

void MapMarkerBatchItem::onCamera(const QGVCameraState& oldState,
                                  const QGVCameraState& newState)
{
    QGVDrawItem::onCamera(oldState, newState);
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

void MapMarkerBatchItem::scheduleCacheRebuild()
{
    if (m_cacheCancelled) {
        m_cacheCancelled->store(true, std::memory_order_relaxed);
    }
    ++m_cacheGeneration;
    m_cacheTimer.start();
}

void MapMarkerBatchItem::rebuildCache()
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
                if (m_overviewCache.isNull()) {
                    m_overviewRefreshTimer.stop();
                    rebuildOverviewCache();
                }
            });
    watcher->setFuture(QtConcurrent::run(
        &MapMarkerBatchItem::renderCache, m_markers, m_projected, m_worldRect,
        cacheRect, cacheScale, m_labelHalo, m_labelText, true, cancelled));
}

void MapMarkerBatchItem::rebuildOverviewCache()
{
    if (m_worldRect.isEmpty()) {
        return;
    }
    const double scale = qMin(
        static_cast<double>(kOverviewCacheDimension) / m_worldRect.width(),
        static_cast<double>(kOverviewCacheDimension) / m_worldRect.height());
    const quint64 generation = ++m_overviewGeneration;
    if (m_overviewCancelled) {
        m_overviewCancelled->store(true, std::memory_order_relaxed);
    }
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
    // Labels belong to the detailed cache. Omitting them here keeps this
    // whole-world fallback quick and uncluttered; its job is to preserve
    // geographic coverage while a post-gesture detail render finishes.
    watcher->setFuture(QtConcurrent::run(
        &MapMarkerBatchItem::renderCache, m_markers, m_projected, m_worldRect,
        m_worldRect, scale, QColor(), QColor(), false, cancelled));
}

MapMarkerBatchItem::CacheResult MapMarkerBatchItem::renderCache(
    QVector<MapView::Marker> markers, QVector<QPointF> projected,
    QRectF worldRect, QRectF cacheRect, double cacheScale,
    QColor labelHalo, QColor labelText, bool renderLabels,
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
    cachePainter.setFont(markerFont());
    const double worldWidth = worldRect.width();

    struct ShapeGroup {
        QColor color;
        bool monitor{false};
        QVector<QPointF> points;
    };
    struct Label {
        int markerIndex{-1};
        QPointF point;
    };
    QVector<ShapeGroup> shapeGroups;
    QVector<Label> labels;
    QHash<quint64, int> groupIndexes;
    for (int i = 0; i < markers.size(); ++i) {
        if (cancelled->load(std::memory_order_relaxed)) {
            cachePainter.end();
            return {};
        }
        const MapView::Marker& marker = markers.at(i);
        const QPointF base = projected.at(i);
        const int firstCopy = static_cast<int>(std::floor(
            (cacheRect.left() - base.x()) / worldWidth));
        const int lastCopy = static_cast<int>(std::ceil(
            (cacheRect.right() - base.x()) / worldWidth));
        for (int copy = firstCopy; copy <= lastCopy; ++copy) {
            const QPointF projected(base.x() + copy * worldWidth, base.y());
            if (!cacheRect.contains(projected)) {
                continue;
            }
            const QPointF point(
                (projected.x() - cacheRect.left()) * cacheScale,
                (projected.y() - cacheRect.top()) * cacheScale);
            const quint64 key = (static_cast<quint64>(marker.color.rgba()) << 1)
                              | (marker.isMonitor ? 1ULL : 0ULL);
            int groupIndex = groupIndexes.value(key, -1);
            if (groupIndex < 0) {
                groupIndex = shapeGroups.size();
                groupIndexes.insert(key, groupIndex);
                ShapeGroup group;
                group.color = marker.color;
                group.monitor = marker.isMonitor;
                shapeGroups.append(group);
            }
            ShapeGroup& group = shapeGroups[groupIndex];
            group.points.append(point);
            if (renderLabels && !marker.label.isEmpty()) {
                labels.append({i, point});
            }
        }
    }

    for (const ShapeGroup& group : std::as_const(shapeGroups)) {
        QColor outline = group.color.darker(220);
        outline.setAlpha(180);
        const double radius = group.monitor ? kMonitorRadius : kDotRadius;
        // A single compound QPainterPath containing thousands of ellipses is
        // expensive for Qt to tessellate, especially when the whole world is
        // visible. Round-cap point batches produce the same circular markers
        // without that nonlinear path-fill cost.
        cachePainter.setPen(QPen(outline, radius * 2.0 + 2.0,
                                 Qt::SolidLine, Qt::RoundCap));
        cachePainter.drawPoints(group.points.constData(), group.points.size());
        cachePainter.setPen(QPen(group.color, radius * 2.0,
                                 Qt::SolidLine, Qt::RoundCap));
        cachePainter.drawPoints(group.points.constData(), group.points.size());
        if (group.monitor) {
            QColor center = group.color.lighter(250);
            center.setAlpha(210);
            cachePainter.setPen(QPen(center, 4.0, Qt::SolidLine,
                                     Qt::RoundCap));
            cachePainter.drawPoints(group.points.constData(),
                                    group.points.size());
        }
    }
    const QFontMetricsF metrics(cachePainter.font());
    for (const Label& label : std::as_const(labels)) {
        if (cancelled->load(std::memory_order_relaxed)) {
            cachePainter.end();
            return {};
        }
        const MapView::Marker& marker = markers.at(label.markerIndex);
        const QRectF labelRect(
            label.point.x() + radiusFor(marker) + kLabelGap,
            label.point.y() - metrics.height() / 2.0,
            metrics.horizontalAdvance(marker.label), metrics.height());
        cachePainter.setPen(labelHalo);
        for (const QPointF delta : {QPointF{1, 0}, QPointF{-1, 0},
                                     QPointF{0, 1}, QPointF{0, -1}}) {
            cachePainter.drawText(labelRect.translated(delta),
                                  Qt::TextSingleLine, marker.label);
        }
        cachePainter.setPen(labelText);
        cachePainter.drawText(labelRect, Qt::TextSingleLine, marker.label);
    }
    cachePainter.end();
    return {cacheRect, cache, cacheScale};
}

QPainterPath MapMarkerBatchItem::projShape() const
{
    QPainterPath shape;
    for (int copy = -kMaxWorldCopies; copy <= kMaxWorldCopies; ++copy) {
        shape.addRect(m_worldRect.translated(copy * m_worldRect.width(), 0.0));
    }
    return shape;
}

void MapMarkerBatchItem::projPaint(QPainter* painter)
{
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

int MapMarkerBatchItem::markerAt(const QPointF& projPos) const
{
    if (getMap() == nullptr || m_worldRect.isEmpty()) {
        return -1;
    }
    const double scale = getMap()->getCamera().scale();
    if (scale <= 0.0) {
        return -1;
    }
    const double threshold = kHitRadius / scale;
    const double thresholdSquared = threshold * threshold;
    const double worldWidth = m_worldRect.width();
    int nearest = -1;
    double nearestSquared = thresholdSquared;
    for (int i = 0; i < m_projected.size(); ++i) {
        QPointF point = m_projected.at(i);
        point.rx() += qRound((projPos.x() - point.x()) / worldWidth)
                    * worldWidth;
        const double dx = point.x() - projPos.x();
        const double dy = point.y() - projPos.y();
        const double distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= nearestSquared) {
            nearestSquared = distanceSquared;
            nearest = i;
        }
    }
    return nearest;
}

} // namespace AetherSDR
