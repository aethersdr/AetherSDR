#pragma once

#include <QGeoView/QGVDrawItem.h>

#include <QColor>
#include <QVector>

namespace AetherSDR {

// Great-circle path from the home station to a reception spot.
// The geodesic is sampled on the sphere and drawn as a thin cosmetic
// polyline; segments crossing the antimeridian are split so the line
// doesn't streak across the whole map.
class MapPathItem : public QGVDrawItem {
    Q_OBJECT

public:
    MapPathItem(double fromLat, double fromLon,
                double toLat, double toLon, const QColor& color);

private:
    void onProjection(QGVMap* geoMap) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;

    double m_fromLat, m_fromLon, m_toLat, m_toLon;
    QColor m_color;
    QPainterPath m_projPath;
};

} // namespace AetherSDR
