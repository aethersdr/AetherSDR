#pragma once

#include <QPainter>
#include <QRectF>

namespace AetherSDR {

// deviceViewport uses physical pixels with a top-left origin (unlike GL's
// bottom-left viewport). The caller must supply a nonempty viewport.
inline QPointF weatherRadarPainterClipPosition(
    const QPainter& painter, const QPointF& itemPoint,
    const QRectF& deviceViewport)
{
    // QPainter's deviceTransform ALREADY includes the device-pixel ratio,
    // plus the map's pan/zoom and any paint-device redirection. Never multiply
    // this point by DPR again: at Retina scale 2, that doubles the overlay's
    // size/offset relative to the basemap, moving US echoes into other continents.
    // Only convert these physical device pixels to GL clip coordinates.
    const QPointF devicePoint = painter.deviceTransform().map(itemPoint);
    return QPointF(
        2.0 * (devicePoint.x() - deviceViewport.left()) / deviceViewport.width() - 1.0,
        1.0 - 2.0 * (devicePoint.y() - deviceViewport.top()) / deviceViewport.height());
}

} // namespace AetherSDR
