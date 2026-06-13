#pragma once

#include <QGeoView/QGVDrawItem.h>

#include "MapView.h"

namespace AetherSDR {

// Pixel-sized (zoom-independent) dot + label marker for MapView.
// Shape is defined in pixel units around projAnchor() — the IgnoreScale
// item flag counter-scales by 1/cameraScale so the drawn size is constant
// on screen at every zoom level.
class MapMarkerItem : public QGVDrawItem {
    Q_OBJECT

public:
    explicit MapMarkerItem(const MapView::Marker& marker);

    void setMarker(const MapView::Marker& marker);
    const MapView::Marker& marker() const { return m_marker; }

    // Sonar-pulse phase for the home marker: 0..1 sweeps one expanding
    // ring, negative disables. Driven by MapView's pulse animation.
    void setPulsePhase(double phase);

private:
    void onProjection(QGVMap* geoMap) override;
    QPainterPath projShape() const override;
    QPointF projAnchor() const override;
    void projPaint(QPainter* painter) override;
    QString projTooltip(const QPointF& projPos) const override;

    QRectF labelRect() const;

    MapView::Marker m_marker;
    QPointF m_projPos;
    double m_pulsePhase{-1.0};
};

} // namespace AetherSDR
