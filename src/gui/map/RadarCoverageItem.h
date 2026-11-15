#pragma once
#include "RadarCoverage.h"
#include <QGeoView/QGVDrawItem.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVProjection.h>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include "core/ThemeManager.h"

namespace AetherSDR {
// One scene item for all sites; avoids thousands of QObject markers.
class RadarCoverageItem final : public QGVDrawItem {
public:
    void setSites(const QVector<RadarSite>& sites) { m_sites = sites; refresh(); }
    QString tooltipAt(const QPointF& point) const
    {
        if (!isVisible() || !getMap()) { return {}; }
        const double world = getMap()->getProjection()->boundaryProjRect().width();
        const double radius = 8 / getMap()->getCamera().scale();
        for (const RadarSite& site : m_sites) {
            QPointF p = getMap()->getProjection()->geoToProj(QGV::GeoPos(site.lat, site.lon));
            p.rx() += std::round((point.x() - p.x()) / world) * world;
            if (QLineF(p, point).length() <= radius) { return site.description(); }
        }
        return {};
    }
private:
    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override
    {
        QGVDrawItem::onCamera(oldState, newState);
        resetBoundary(); refresh();
    }
    QPainterPath projShape() const override
    {
        QPainterPath p;
        if (getMap()) { p.addRect(getMap()->getCamera().projRect()); }
        return p;
    }
    void projPaint(QPainter* painter) override
    {
        if (!getMap()) { return; }
        const QGVProjection* projection = getMap()->getProjection();
        const double world = projection->boundaryProjRect().width();
        const QRectF view = getMap()->getCamera().projRect();
        QColor color = ThemeManager::instance().color("color.text.secondary");
        color.setAlpha(12);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        QPainterPath coverage;
        coverage.setFillRule(Qt::WindingFill);
        for (const RadarSite& site : m_sites) {
            const QPointF center = projection->geoToProj(QGV::GeoPos(site.lat, site.lon));
            QPainterPath ring;
            double previousX = center.x();
            bool first = true;
            for (const QPointF& geo : site.ring) {
                QPointF p = projection->geoToProj(QGV::GeoPos(std::clamp(geo.y(), -85.0, 85.0), geo.x()));
                p.rx() += std::round((previousX - p.x()) / world) * world;
                if (first) { ring.moveTo(p); first = false; } else { ring.lineTo(p); }
                previousX = p.x();
            }
            const int nearest = qRound((view.center().x() - center.x()) / world);
            for (int copy = nearest - 1; copy <= nearest + 1; ++copy) {
                coverage.addPath(ring.translated(copy * world, 0));
            }
        }
        // One fill keeps intersecting ranges at the same subtle opacity.
        painter->drawPath(coverage);
    }
    QVector<RadarSite> m_sites;
};
}
