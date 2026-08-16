#include "MapPathItem.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVProjection.h>

#include <QPainter>
#include <QtMath>

#include <limits>

namespace AetherSDR {

namespace {
constexpr int kSegments = 32;

// Spherical linear interpolation between two lat/lon points (degrees).
QGV::GeoPos slerp(double lat1, double lon1, double lat2, double lon2,
                  double t)
{
    const double p1 = qDegreesToRadians(lat1);
    const double l1 = qDegreesToRadians(lon1);
    const double p2 = qDegreesToRadians(lat2);
    const double l2 = qDegreesToRadians(lon2);

    const double d = 2.0
        * std::asin(std::sqrt(
            std::pow(std::sin((p1 - p2) / 2.0), 2)
            + std::cos(p1) * std::cos(p2)
                  * std::pow(std::sin((l1 - l2) / 2.0), 2)));
    if (d < 1e-9) {
        return { lat1, lon1 };
    }
    const double a = std::sin((1.0 - t) * d) / std::sin(d);
    const double b = std::sin(t * d) / std::sin(d);
    const double x = a * std::cos(p1) * std::cos(l1)
                   + b * std::cos(p2) * std::cos(l2);
    const double y = a * std::cos(p1) * std::sin(l1)
                   + b * std::cos(p2) * std::sin(l2);
    const double z = a * std::sin(p1) + b * std::sin(p2);
    return { qRadiansToDegrees(std::atan2(z, std::sqrt(x * x + y * y))),
             qRadiansToDegrees(std::atan2(y, x)) };
}
} // namespace

MapPathItem::MapPathItem(double fromLat, double fromLon,
                         double toLat, double toLon, const QColor& color,
                         int relativeWorldOffset)
    : m_fromLat(fromLat)
    , m_fromLon(fromLon)
    , m_toLat(toLat)
    , m_toLon(toLon)
    , m_relativeWorldOffset(relativeWorldOffset)
    , m_color(color)
{
    setSelectable(false);
    setZValue(-1);  // under the markers
}

void MapPathItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);

    const QGVProjection* proj = geoMap->getProjection();
    // Antimeridian guard: a projected-x jump of more than half the world
    // width means the great circle wrapped — start a new subpath.
    const double worldWidth = proj->boundaryProjRect().width();

    m_baseProjPath = QPainterPath();
    QPointF prev;
    for (int i = 0; i <= kSegments; ++i) {
        const double t = static_cast<double>(i) / kSegments;
        const QPointF p = proj->geoToProj(
            slerp(m_fromLat, m_fromLon, m_toLat, m_toLon, t));
        if (i == 0 || std::abs(p.x() - prev.x()) > worldWidth / 2.0) {
            m_baseProjPath.moveTo(p);
        } else {
            m_baseProjPath.lineTo(p);
        }
        prev = p;
    }
    // Anchor the world-copy choice on the DESTINATION endpoint — exactly the
    // point MapMarkerItem rounds against for the spot this path terminates at.
    // The path's bounding-rect centre is not usable: a great circle crossing
    // the antimeridian is split into subpaths hard against both edges, so its
    // centre collapses toward the projection origin and bears no relation to
    // either endpoint. Rounding the two against different anchors makes them
    // disagree about which copies exist over a wide band of camera positions,
    // and a spot ends up drawn with no path running to it.
    m_baseAnchor = proj->geoToProj(QGV::GeoPos(m_toLat, m_toLon));
    m_appliedWorldOffset = std::numeric_limits<int>::min();
    m_projPath = m_baseProjPath;
    onCamera(geoMap->getCamera(), geoMap->getCamera());
}

void MapPathItem::onCamera(const QGVCameraState& oldState,
                           const QGVCameraState& newState)
{
    const double worldWidth = newState.getProjection()->boundaryProjRect().width();
    const int worldOffset = m_relativeWorldOffset + qRound(
        (newState.projCenter().x() - m_baseAnchor.x()) / worldWidth);
    // Compare the integer copy index rather than the mapped path. The offset
    // only changes when the camera crosses a world boundary, so an ordinary
    // drag frame costs two doubles here instead of a QTransform::map() over
    // every segment plus a full QPainterPath comparison — per item, at several
    // items per spot, on the very drag path this change exists to keep clear.
    if (worldOffset != m_appliedWorldOffset) {
        m_appliedWorldOffset = worldOffset;
        m_projPath = QTransform::fromTranslate(
            worldOffset * worldWidth, 0.0).map(m_baseProjPath);
        resetBoundary();
        refresh();
    }
    QGVDrawItem::onCamera(oldState, newState);
}

QPainterPath MapPathItem::projShape() const
{
    return m_projPath;
}

void MapPathItem::projPaint(QPainter* painter)
{
    QColor c = m_color;
    c.setAlpha(120);
    QPen pen(c, 2.5);
    pen.setCosmetic(true);  // constant pixel width at every zoom
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(m_projPath);
}

} // namespace AetherSDR
