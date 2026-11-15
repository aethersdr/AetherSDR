#pragma once

#include "SmartMtrStyle.h"

#include <QPointF>
#include <QRect>
#include <QRectF>

#include <algorithm>

namespace AetherSDR {

// SmartMTR geometry is expressed in abstract UNITS (constants in
// SmartMtrStyle.h), never pixels. The kControlW x kControlH design area is
// fit-and-centered with one uniform scale (pxPerUnit = pixelExtent / unitExtent),
// leftover space transparent letterbox. This struct is the only place pxPerUnit
// is computed; all drawing goes through g.rect(...) / g.len(...).
struct SmartMtrGeometry {
    double pxPerUnit{1.0};
    double originX{0.0}; // pixel offset of the design's top-left (letterbox)
    double originY{0.0};

    // Build the mapping that fits the kControlW x kControlH design, centered,
    // into the given widget pixel rect.
    static SmartMtrGeometry fit(const QRect& widgetRect);

    // A length in UNITS -> pixels.
    double len(double units) const { return units * pxPerUnit; }

    // A point given in UNITS (x,y) -> pixel QPointF, through the same mapping as
    // rect(). Used for polygon vertices (e.g. the extreme-marker triangles).
    QPointF point(double x, double y) const
    {
        return QPointF(originX + x * pxPerUnit, originY + y * pxPerUnit);
    }

    // A rectangle given in UNITS (top-left x,y and size w,h) -> pixel QRectF.
    QRectF rect(double x, double y, double w, double h) const
    {
        return QRectF(originX + x * pxPerUnit, originY + y * pxPerUnit,
                      w * pxPerUnit, h * pxPerUnit);
    }
};

inline SmartMtrGeometry SmartMtrGeometry::fit(const QRect& widgetRect)
{
    const double w = widgetRect.width();
    const double h = widgetRect.height();

    SmartMtrGeometry g;
    // Uniform scale that fits the whole design area in both axes.
    g.pxPerUnit = std::min(w / SmartMtrUnits::kControlW,
                           h / SmartMtrUnits::kControlH);
    // Center the scaled design in the widget; leftover is transparent letterbox.
    g.originX = widgetRect.x() + (w - SmartMtrUnits::kControlW * g.pxPerUnit) / 2.0;
    g.originY = widgetRect.y() + (h - SmartMtrUnits::kControlH * g.pxPerUnit) / 2.0;
    return g;
}

} // namespace AetherSDR
