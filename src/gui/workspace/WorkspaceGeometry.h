#pragma once

// Normalized canvas geometry (RFC #4887): items store placement as fractions of
// their surface, never pixels, so a layout restores across display sizes.
// Widget-free; see tests/workspace_geometry_test.cpp.

#include <QMetaType>
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

// Map a normalized rect onto a pixel canvas. Each edge is rounded
// independently so items sharing a normalized edge share a pixel column at every
// size (no hairline seams). Sub-pixel rects may yield zero size; callers clamp
// through clampToCanvas() with a minimum.
QRect toPixels(const NormRect& r, const QSize& canvas);

// Map a floating window's GLOBAL rect into canvas fractions (phase 6's
// "import pop-outs"): translate into the canvas's global frame, normalize,
// then clampToBounds() so an off-screen or oversized window still lands as
// a usable item.  Pure; returns a default NormRect when either rect is
// degenerate.
NormRect normRectFromGlobal(const QRect& windowGlobal, const QRect& canvasGlobal);

// Inverse of toPixels().  Returns a default-constructed NormRect for a
// degenerate canvas rather than dividing by zero.
NormRect fromPixels(const QRect& px, const QSize& canvas);

// The smallest normalized size that still measures at least `minPx` on this
// canvas, capped at the full canvas.  On a canvas smaller than the minimum the
// cap wins and the item fills the surface — a too-small item is a worse
// outcome than one that overflows a tiny window.
QSizeF minimumNormSize(const QSize& minPx, const QSize& canvas);

// Bring `r` inside the unit square, size bounded to (0,1] with no growth; the
// only clamp the model may apply. Pixel-free so stored rects never depend on
// the canvas size at write time (a pixel clamp before layout would inflate every
// item). Minimum size is enforced only by clampToCanvas() at display time.
NormRect clampToBounds(const NormRect& r);

// Display-only clamp into the canvas: grow to minimum, cap at the canvas, then
// translate inside (so an overshooting drag slides rather than resizes). A
// degenerate canvas returns `r` untouched. Never store the result: it would bake
// one window size's compromises into the arrangement.
NormRect clampToCanvas(const NormRect& r, const QSize& minPx, const QSize& canvas);

}  // namespace AetherSDR

// Signal parameter in WorkspaceCanvas (gestureStarted); registered so
// QSignalSpy and queued connections can carry it.
Q_DECLARE_METATYPE(AetherSDR::NormRect)
