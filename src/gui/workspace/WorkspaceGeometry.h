#pragma once

// Normalized canvas geometry — the pure half of the workspace canvas
// (RFC #4887, phase 1).
//
// Canvas items store their placement as fractions of the surface they sit on,
// never as pixels.  A layout built on a 3840x1600 display therefore restores
// correctly on a 1920x1080 laptop: only the cell pitch changes.  Pixel
// geometry cannot do that, and is half of why pop-out window restore feels
// random today (see the RFC's Problem section).
//
// Nothing here knows what a QWidget is, so all of it is unit-tested headless
// on every platform — see tests/workspace_geometry_test.cpp.

#include <QRect>
#include <QSize>
#include <QSizeF>

namespace AetherSDR {

// A rectangle in canvas-relative coordinates: x/y is the top-left corner as a
// fraction of the canvas, w/h the size as a fraction of it.  A rect fully
// inside its canvas satisfies 0 <= x, x + w <= 1 (and likewise for y/h), but
// the type itself does not enforce that — clampToCanvas() does, because a
// caller mid-drag legitimately holds a rect that is briefly outside.
struct NormRect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    double right()  const { return x + w; }
    double bottom() const { return y + h; }

    // Finite, and with a strictly positive size.  A zero-area rect is not
    // valid: it would map to an invisible item that still answers hit tests.
    bool isValid() const;

    // Fraction-space containment, half-open on the far edges so two items
    // sharing an edge do not both claim a point on it.
    bool contains(double px, double py) const;

    bool operator==(const NormRect& o) const;
    bool operator!=(const NormRect& o) const { return !(*this == o); }
};

// Map a normalized rect onto a pixel canvas.
//
// Each EDGE is rounded independently rather than rounding the origin and the
// size: two items that share a normalized edge then land on the same pixel
// column at every canvas size, so a tiled arrangement neither gaps nor
// overlaps by a pixel.  Rounding origin+size instead lets the shared edge fall
// on two different pixels, which shows up as a hairline seam that moves as the
// window is resized.
//
// Sub-pixel rects can round to a zero-width or zero-height QRect; this
// function does not silently inflate them.  Callers keep items measurable by
// clamping through clampToCanvas() with a minimum pixel size.
QRect toPixels(const NormRect& r, const QSize& canvas);

// Inverse of toPixels().  Returns a default-constructed NormRect for a
// degenerate canvas rather than dividing by zero.
NormRect fromPixels(const QRect& px, const QSize& canvas);

// The smallest normalized size that still measures at least `minPx` on this
// canvas, capped at the full canvas.  On a canvas smaller than the minimum the
// cap wins and the item fills the surface — a too-small item is a worse
// outcome than one that overflows a tiny window.
QSizeF minimumNormSize(const QSize& minPx, const QSize& canvas);

// Bring `r` inside the canvas, preserving its size wherever possible.
//
// Order matters, and is chosen so a drag that overshoots an edge slides the
// item back rather than resizing it under the operator's cursor:
//   1. grow to the minimum size, then cap at the canvas — the cap is also the
//      shrink case, for an item bigger than the surface,
//   2. translate back inside.
//
// A degenerate canvas returns `r` untouched: with no surface to clamp against
// there is no correct answer, and mangling the stored layout because a widget
// has not been shown yet would lose real state.
NormRect clampToCanvas(const NormRect& r, const QSize& minPx, const QSize& canvas);

}  // namespace AetherSDR
