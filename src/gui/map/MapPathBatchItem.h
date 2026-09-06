#pragma once

#include "MapView.h"

#include <QGeoView/QGVDrawItem.h>

#include <QImage>
#include <QFutureWatcher>
#include <QPainterPath>
#include <QTimer>

#include <atomic>
#include <memory>

class QGVProjection;

namespace AetherSDR {

class MapBatchItemTestAccess;

class MapPathBatchItem : public QGVDrawItem {
    Q_OBJECT

public:
    MapPathBatchItem(const QVector<MapView::Marker>& markers,
                     bool hasHome, double homeLat, double homeLon);
    ~MapPathBatchItem() override;
    void setMarkers(const QVector<MapView::Marker>& markers,
                    bool hasHome, double homeLat, double homeLon);
    void setDisplayVisible(bool visible);

private:
    friend class MapBatchItemTestAccess;
    struct CacheResult {
        QRectF rect;
        QImage image;
        double scale{0.0};
    };

    struct Path {
        QPainterPath projected;
        QRectF bounds;
        QColor color;
    };

    void onProjection(QGVMap* geoMap) override;
    void onCamera(const QGVCameraState& oldState,
                  const QGVCameraState& newState) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;
    void rebuildCache();
    void rebuildOverviewCache();
    void scheduleCacheRebuild();
    void rebuildProjectedPaths();
    static QVector<Path> projectPaths(
        QVector<MapView::Marker> markers, bool hasHome,
        double homeLat, double homeLon, QRectF worldRect,
        std::shared_ptr<std::atomic_bool> cancelled);
    static CacheResult renderCache(QVector<Path> paths,
                                   QRectF worldRect, QRectF cacheRect,
                                   double cacheScale,
                                   std::shared_ptr<std::atomic_bool> cancelled);

    QVector<MapView::Marker> m_markers;
    QVector<Path> m_paths;
    bool m_directPaint{false};
    bool m_hasHome{false};
    double m_homeLat{0.0};
    double m_homeLon{0.0};
    QRectF m_worldRect;
    QRectF m_cacheRect;
    QImage m_cache;
    double m_cacheScale{0.0};
    QImage m_overviewCache;
    QTimer m_cacheTimer;
    QTimer m_overviewRefreshTimer;
    quint64 m_cacheGeneration{0};
    quint64 m_overviewGeneration{0};
    quint64 m_projectionGeneration{0};
    std::shared_ptr<std::atomic_bool> m_cacheCancelled;
    std::shared_ptr<std::atomic_bool> m_overviewCancelled;
    std::shared_ptr<std::atomic_bool> m_projectionCancelled;
};

} // namespace AetherSDR
