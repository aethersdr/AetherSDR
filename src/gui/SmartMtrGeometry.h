#pragma once

#include "SmartMtrStyle.h"

#include <QPointF>
#include <QRect>
#include <QRectF>

#include <algorithm>

namespace AetherSDR {

// ============================================================================
// UNITS vs. PIXELS  —  the SmartMTR coordinate system
// ============================================================================
//
// All SmartMTR geometry (bar lengths, tick positions, gaps, line thickness,
// margins, ...) is expressed in *UNITS*, never in raw pixels (see
// SmartMtrStyle.h for the unit constants).
//
// UNITS are abstract, resolution-independent design units that define only the
// *proportions* of the control. They have no fixed pixel size of their own.
//
// At paint time UNITS are converted to pixels with a single scale factor:
//
//       pxPerUnit = actualPixelExtent / totalUnitExtent
//       pixels    = units * pxPerUnit
//
// The design area is SmartMtrUnits::kControlW x kControlH. Because the widget's
// real pixel rect may not share that aspect ratio, we fit-and-center: one
// uniform scale factor for both axes (aspect ratio preserved, UNITS square),
// with the design centered and the leftover margin left as transparent
// letterbox.
//
// This struct is the single authority for that mapping. Every element draws
// through it (g.rect(...) / g.len(...)); no drawing code computes pxPerUnit
// itself.
// ============================================================================
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
