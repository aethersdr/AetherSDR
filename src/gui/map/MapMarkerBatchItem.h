#pragma once

#include "MapView.h"

#include <QGeoView/QGVDrawItem.h>

#include <QColor>
#include <QImage>
#include <QFutureWatcher>
#include <QTimer>

#include <atomic>
#include <memory>

class QGVProjection;

namespace AetherSDR {

class MapBatchItemTestAccess;

// Draws every non-home marker in one scene item.  Thousands of individual
// QGVDrawItems make every camera change walk thousands of QObject/item
// boundaries; this batch keeps the same marker data and hit testing while the
// scene only has one camera participant.
class MapMarkerBatchItem : public QGVDrawItem {
    Q_OBJECT

public:
    MapMarkerBatchItem(const QVector<MapView::Marker>& markers,
                       const QColor& labelHalo, const QColor& labelText);
    ~MapMarkerBatchItem() override;

    void setMarkers(const QVector<MapView::Marker>& markers);
    int markerAt(const QPointF& projPos) const;
    const MapView::Marker& marker(int index) const { return m_markers.at(index); }

private:
    friend class MapBatchItemTestAccess;
    struct CacheResult {
        QRectF rect;
        QImage image;
        double scale{0.0};
    };

    void onProjection(QGVMap* geoMap) override;
    void onCamera(const QGVCameraState& oldState,
                  const QGVCameraState& newState) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;
    void rebuildCache();
    void rebuildOverviewCache();
    void scheduleCacheRebuild();
    void projectMarkers(const QGVProjection* projection);
    static CacheResult renderCache(QVector<MapView::Marker> markers,
                                   QVector<QPointF> projected,
                                   QRectF worldRect, QRectF cacheRect,
                                   double cacheScale, QColor labelHalo,
                                   QColor labelText, bool renderLabels,
                                   std::shared_ptr<std::atomic_bool> cancelled);

    QVector<MapView::Marker> m_markers;
    QVector<QPointF> m_projected;
    QColor m_labelHalo;
    QColor m_labelText;
    QRectF m_worldRect;
    QRectF m_cacheRect;
    QImage m_cache;
    double m_cacheScale{0.0};
    QImage m_overviewCache;
    QTimer m_cacheTimer;
    QTimer m_overviewRefreshTimer;
    quint64 m_cacheGeneration{0};
    quint64 m_overviewGeneration{0};
    std::shared_ptr<std::atomic_bool> m_cacheCancelled;
    std::shared_ptr<std::atomic_bool> m_overviewCancelled;
};

} // namespace AetherSDR
