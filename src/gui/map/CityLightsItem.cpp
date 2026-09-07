#include "CityLightsItem.h"
#include "WeatherRadarWorldWrap.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGraphicsScene>
#include <QPainter>
#include <QPainterPath>

namespace AetherSDR {

CityLightsItem::CityLightsItem()
{
    setSelectable(false);
}

void CityLightsItem::setImage(const QImage& image, const QRectF& canonicalBounds)
{
    if (m_image.cacheKey() == image.cacheKey()
        && m_bounds == QRectF(canonicalBounds.left(), -canonicalBounds.bottom(),
                              canonicalBounds.width(), canonicalBounds.height())) {
        return;
    }
    m_image = image;
    m_bounds = QRectF(canonicalBounds.left(), -canonicalBounds.bottom(),
                     canonicalBounds.width(), canonicalBounds.height());
    if (getMap() != nullptr) {
        m_camera = getMap()->getCamera().projRect().normalized();
    }
    resetBoundary();
    refresh();
}

void CityLightsItem::onProjection(QGVMap* map)
{
    QGVDrawItem::onProjection(map);
    m_camera = map->getCamera().projRect().normalized();
    // Like the radar raster path, keep the original QImage cache identity;
    // viewport-sized device caches churn on every pan and playback repaint.
    for (QGraphicsItem* item : map->geoView()->scene()->items()) {
        if (QGVMapQGItem::geoObjectFromQGItem(item) == this) {
            item->setCacheMode(QGraphicsItem::NoCache);
            break;
        }
    }
}

void CityLightsItem::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    const QRectF next = newState.projRect().normalized();
    const bool changed = weatherRadarVisibleWorldOffsets(m_bounds, m_camera)
        != weatherRadarVisibleWorldOffsets(m_bounds, next);
    m_camera = next;
    if (changed) {
        resetBoundary();
        refresh();
    }
    QGVDrawItem::onCamera(oldState, newState);
}

QPainterPath CityLightsItem::projShape() const
{
    QPainterPath path;
    for (double offset : weatherRadarVisibleWorldOffsets(m_bounds, m_camera)) {
        path.addRect(m_bounds.translated(offset, 0));
    }
    return path;
}

void CityLightsItem::projPaint(QPainter* painter)
{
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (double offset : weatherRadarVisibleWorldOffsets(m_bounds, m_camera)) {
        painter->drawImage(m_bounds.translated(offset, 0), m_image);
    }
}

} // namespace AetherSDR
