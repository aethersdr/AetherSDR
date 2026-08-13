#include "gui/workspace/WorkspaceGeometry.h"

#include <QtGlobal>

#include <cmath>

namespace AetherSDR {

namespace {

// Fraction-space slack.  Placement is authored by dragging, stored as a
// double, and compared after a pixel round-trip, so exact equality is the
// wrong test.  1e-9 is far below one pixel on any canvas we will ever have
// (1e-9 of 3840 px is 4e-6 px) and far above double's rounding noise.
constexpr double kEpsilon = 1e-9;

bool finite(double v)
{
    return std::isfinite(v);
}

double clampDouble(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool canvasUsable(const QSize& canvas)
{
    return canvas.width() > 0 && canvas.height() > 0;
}

}  // namespace

bool NormRect::isValid() const
{
    return finite(x) && finite(y) && finite(w) && finite(h)
           && w > 0.0 && h > 0.0;
}

bool NormRect::contains(double px, double py) const
{
    return px >= x && px < right() && py >= y && py < bottom();
}

bool NormRect::operator==(const NormRect& o) const
{
    return std::fabs(x - o.x) <= kEpsilon
           && std::fabs(y - o.y) <= kEpsilon
           && std::fabs(w - o.w) <= kEpsilon
           && std::fabs(h - o.h) <= kEpsilon;
}

QRect toPixels(const NormRect& r, const QSize& canvas)
{
    if (!canvasUsable(canvas) || !finite(r.x) || !finite(r.y)
        || !finite(r.w) || !finite(r.h)) {
        return {};
    }

    const double cw = canvas.width();
    const double ch = canvas.height();

    // Round the edges, not the origin and the size — see the header.
    const int left   = qRound(r.x * cw);
    const int top    = qRound(r.y * ch);
    const int right  = qRound(r.right() * cw);
    const int bottom = qRound(r.bottom() * ch);

    return QRect(left, top, right - left, bottom - top);
}

NormRect normRectFromGlobal(const QRect& windowGlobal, const QRect& canvasGlobal)
{
    if (windowGlobal.isEmpty() || canvasGlobal.isEmpty()) {
        return NormRect{};
    }
    NormRect r;
    r.x = (windowGlobal.x() - canvasGlobal.x())
          / double(canvasGlobal.width());
    r.y = (windowGlobal.y() - canvasGlobal.y())
          / double(canvasGlobal.height());
    r.w = windowGlobal.width()  / double(canvasGlobal.width());
    r.h = windowGlobal.height() / double(canvasGlobal.height());
    return clampToBounds(r);
}

NormRect fromPixels(const QRect& px, const QSize& canvas)
{
    if (!canvasUsable(canvas)) {
        return {};
    }

    const double cw = canvas.width();
    const double ch = canvas.height();

    NormRect r;
    r.x = px.x() / cw;
    r.y = px.y() / ch;
    r.w = px.width() / cw;
    r.h = px.height() / ch;
    return r;
}

QSizeF minimumNormSize(const QSize& minPx, const QSize& canvas)
{
    if (!canvasUsable(canvas)) {
        return {};
    }

    const double w = qMax(0, minPx.width())  / static_cast<double>(canvas.width());
    const double h = qMax(0, minPx.height()) / static_cast<double>(canvas.height());
    return QSizeF(qMin(w, 1.0), qMin(h, 1.0));
}

NormRect clampToBounds(const NormRect& r)
{
    if (!finite(r.x) || !finite(r.y) || !finite(r.w) || !finite(r.h)) {
        return {};
    }

    NormRect out = r;
    // Size bounded, never grown: growth is a display concern.
    out.w = clampDouble(out.w, 0.0, 1.0);
    out.h = clampDouble(out.h, 0.0, 1.0);
    // Position slides inside, same order as the display clamp: an overshoot
    // moves the item back rather than resizing it.
    out.x = clampDouble(out.x, 0.0, 1.0 - out.w);
    out.y = clampDouble(out.y, 0.0, 1.0 - out.h);
    return out;
}

NormRect clampToCanvas(const NormRect& r, const QSize& minPx, const QSize& canvas)
{
    if (!canvasUsable(canvas)) {
        return r;
    }
    if (!finite(r.x) || !finite(r.y) || !finite(r.w) || !finite(r.h)) {
        return {};
    }

    const QSizeF minimum = minimumNormSize(minPx, canvas);

    NormRect out = r;

    // 1. Grow to the minimum, then cap at the canvas.  The cap IS the
    //    shrink case: an item larger than the surface, or a minimum that a
    //    tiny canvas cannot honour, ends up exactly canvas-sized.
    out.w = clampDouble(qMax(out.w, minimum.width()),  0.0, 1.0);
    out.h = clampDouble(qMax(out.h, minimum.height()), 0.0, 1.0);

    // 2. Translate back inside.  Step 1 leaves w/h in [0,1], so the upper
    //    bound here is never negative.
    out.x = clampDouble(out.x, 0.0, 1.0 - out.w);
    out.y = clampDouble(out.y, 0.0, 1.0 - out.h);

    return out;
}

}  // namespace AetherSDR
