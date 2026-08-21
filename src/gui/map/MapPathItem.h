#pragma once

#include <QGeoView/QGVDrawItem.h>

#include <QColor>
#include <QVector>

#include <limits>

namespace AetherSDR {

// Great-circle path from the home station to a reception spot.
// The geodesic is sampled on the sphere and drawn as a thin cosmetic
// polyline; segments crossing the antimeridian are split so the line
// doesn't streak across the whole map.
class MapPathItem : public QGVDrawItem {
    Q_OBJECT

public:
    MapPathItem(double fromLat, double fromLon,
                double toLat, double toLon, const QColor& color,
                int relativeWorldOffset = 0);

private:
    void onProjection(QGVMap* geoMap) override;
    void onCamera(const QGVCameraState& oldState,
                  const QGVCameraState& newState) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;

    double m_fromLat, m_fromLon, m_toLat, m_toLon;
    int m_relativeWorldOffset{0};
    QColor m_color;
    QPainterPath m_baseProjPath;
    QPainterPath m_projPath;
    // Projected destination endpoint: the anchor the world-copy index is
    // rounded against, shared with the MapMarkerItem for the same spot.
    QPointF m_baseAnchor;
    // Copy index currently baked into m_projPath. Seeded below any reachable
    // value so the first onCamera() after onProjection() always applies.
    int m_appliedWorldOffset{std::numeric_limits<int>::min()};
};

} // namespace AetherSDR
