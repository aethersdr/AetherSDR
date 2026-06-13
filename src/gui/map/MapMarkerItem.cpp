#include "MapMarkerItem.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVProjection.h>

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>

namespace AetherSDR {

namespace {
constexpr double kDotRadius = 5.0;       // px
constexpr double kHomeRadius = 7.0;      // px
constexpr double kLabelOffset = 3.0;     // px gap between dot and label
constexpr double kPulseMaxRadius = 26.0; // px, sonar ring sweep extent

QFont markerFont()
{
    QFont f;
    f.setPointSizeF(9.0);
    f.setBold(true);
    return f;
}
} // namespace

MapMarkerItem::MapMarkerItem(const MapView::Marker& marker)
    : m_marker(marker)
{
    setFlags(QGV::ItemFlag::IgnoreScale | QGV::ItemFlag::IgnoreAzimuth
             | QGV::ItemFlag::Clickable);
    setSelectable(false);
}

void MapMarkerItem::setMarker(const MapView::Marker& marker)
{
    m_marker = marker;
    if (getMap() != nullptr) {
        onProjection(getMap());
    }
    resetBoundary();
    refresh();
}

void MapMarkerItem::setPulsePhase(double phase)
{
    if (qFuzzyCompare(m_pulsePhase, phase)) {
        return;
    }
    m_pulsePhase = phase;
    repaint();
}

void MapMarkerItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    m_projPos = geoMap->getProjection()->geoToProj(
        QGV::GeoPos(m_marker.lat, m_marker.lon));
}

QPointF MapMarkerItem::projAnchor() const
{
    return m_projPos;
}

QRectF MapMarkerItem::labelRect() const
{
    if (m_marker.label.isEmpty()) {
        return {};
    }
    const QFontMetricsF fm(markerFont());
    const QSizeF size = fm.size(Qt::TextSingleLine, m_marker.label);
    const double r = m_marker.isHome ? kHomeRadius : kDotRadius;
    return { m_projPos.x() + r + kLabelOffset,
             m_projPos.y() - size.height() / 2.0,
             size.width(), size.height() };
}

QPainterPath MapMarkerItem::projShape() const
{
    const double r = m_marker.isHome ? kHomeRadius : kDotRadius;
    QPainterPath path;
    path.addEllipse(m_projPos, r, r);
    if (m_marker.isHome) {
        // Boundary must cover the full sonar ring sweep.
        path.addEllipse(m_projPos, kPulseMaxRadius, kPulseMaxRadius);
    }
    const QRectF lbl = labelRect();
    if (!lbl.isNull()) {
        path.addRect(lbl);
    }
    return path;
}

void MapMarkerItem::projPaint(QPainter* painter)
{
    painter->setRenderHint(QPainter::Antialiasing);
    const double r = m_marker.isHome ? kHomeRadius : kDotRadius;

    if (m_marker.isHome && m_pulsePhase >= 0.0) {
        // Sonar pulse: expanding, fading ring.
        const double radius = kHomeRadius
            + (kPulseMaxRadius - kHomeRadius - 1.0) * m_pulsePhase;
        const int alpha = static_cast<int>(180.0 * (1.0 - m_pulsePhase));
        QColor ring = m_marker.color;
        ring.setAlpha(alpha);
        painter->setPen(QPen(ring, 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(m_projPos, radius, radius);
    }

    if (m_marker.isHome) {
        // Station marker: ringed dot for visual distinction.
        painter->setPen(QPen(m_marker.color, 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(m_projPos, r, r);
        painter->setBrush(m_marker.color);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(m_projPos, r / 2.5, r / 2.5);
    } else {
        painter->setPen(QPen(QColor(0, 0, 0, 160), 1.0));
        painter->setBrush(m_marker.color);
        painter->drawEllipse(m_projPos, r, r);
    }

    if (!m_marker.label.isEmpty()) {
        const QRectF lbl = labelRect();
        painter->setFont(markerFont());
        // Halo for readability on any tile background.
        painter->setPen(QColor(255, 255, 255, 220));
        for (const QPointF d : { QPointF{1, 0}, QPointF{-1, 0},
                                 QPointF{0, 1}, QPointF{0, -1} }) {
            painter->drawText(lbl.translated(d), Qt::TextSingleLine,
                              m_marker.label);
        }
        painter->setPen(QColor(20, 20, 20));
        painter->drawText(lbl, Qt::TextSingleLine, m_marker.label);
    }
}

QString MapMarkerItem::projTooltip(const QPointF& projPos) const
{
    Q_UNUSED(projPos);
    return m_marker.tooltip;
}

} // namespace AetherSDR
