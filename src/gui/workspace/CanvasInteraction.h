#pragma once

// The pure half of canvas interaction (RFC #4887 phase 5): where a press
// lands (move strip, resize grip, nothing), and what a drag does to a rect.
//
// Widget-free on purpose, like the rest of the model: every rule that can be
// got wrong — which grip a corner press selects, which edge a resize
// anchors, how minimums fight bounds — is decided here and pinned headless.
// WorkspaceCanvas's overlay translates real mouse events into these calls
// and applies the answers; it adds no rules of its own.
//
// All rect math is in normalized canvas space.  Pixel thresholds (grip size,
// title strip height, snap tolerance) are converted by the caller against
// the live canvas — interactions only happen on a real, laid-out canvas, so
// unlike stored placement this code MAY know the canvas size; it just never
// writes anything an operator did not do.

#include "gui/workspace/CanvasItem.h"
#include "gui/workspace/WorkspaceGeometry.h"

#include <QString>
#include <QStringList>

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QSizeF>

#include <Qt>

namespace AetherSDR {

// Where a press lands on an item.  Move is the title strip; the eight
// resize zones are bands just inside the item's edges.
enum class HitZone {
    None,
    Move,
    N, S, E, W,
    NE, NW, SE, SW,
};

// Classify a point against an item's pixel rect.  `gripPx` is the resize
// band width; `titleHPx` the height of the move strip below the top edge.
// Corners win over edges (a press within two grip bands is a corner), and
// the grip bands win over the title strip — otherwise the top corners would
// be unreachable on short items.  A point outside the rect is None.
HitZone hitZoneFor(const QRect& itemPx, const QPoint& posPx,
                   int gripPx, int titleHPx);

// The cursor an operator should see over a zone.
Qt::CursorShape cursorForZone(HitZone zone);

bool isResizeZone(HitZone zone);

// Apply a drag to a rect.
//
// Move translates and then slides back inside the unit square (an overshoot
// moves the item, never resizes it).  Resize moves ONLY the gripped edges —
// the opposite edge is the anchor and must not budge, which is the property
// an operator's hands rely on — respecting `minNorm` against the anchored
// edge and capping at the surface.  The result is always inside the unit
// square and at least the minimum on both axes.
NormRect applyDrag(const NormRect& start, HitZone zone,
                   double dxNorm, double dyNorm, const QSizeF& minNorm);

// ── Snapping ─────────────────────────────────────────────────────────────
//
// Candidates are the canvas edges and every peer's edges and centres.  Only
// the edges a gesture actually moves participate: a Move snaps its left,
// right and centre lines; an E resize snaps only its right edge — snapping
// an anchored edge would yank the item out from under the cursor.

struct SnapResult {
    NormRect rect;
    // Guide lines to draw while the gesture holds a snap, in canvas
    // fractions.  Empty when nothing snapped.
    QList<double> verticalGuides;    // x positions
    QList<double> horizontalGuides;  // y positions
};

// Snap `moved` (the rect applyDrag produced) against `peers`.  `tolNorm*`
// is the capture distance per axis, already converted from pixels by the
// caller; disabled (e.g. Alt held) is expressed by calling with zero
// tolerance or simply not calling.  Resize snaps re-apply the minimum so a
// snap can never shrink an item below it.
// `gridX`/`gridY` add a normalized grid (that many divisions per axis) to
// the candidates; 0 disables it.  Two deliberate asymmetries against the
// peer tier: the grid is consulted only when NO peer or surface edge is in
// tolerance (aligning to a neighbour is more intentional than a grid line),
// and for a move it courts only the item's ORIGIN — left/right/centre all
// chasing a ~35 px grid at +/-8 px left almost nowhere free to stand.
// `gridTolNormX/Y` give the grid tier its own capture distance (default:
// same as the peer tier).  A dense grid with a full-strength magnet
// quantizes nearly every position; callers pair high division counts with a
// gentler pull.
SnapResult snapRect(const NormRect& moved, HitZone zone,
                    const QList<NormRect>& peers,
                    double tolNormX, double tolNormY,
                    const QSizeF& minNorm,
                    int gridX = 0, int gridY = 0,
                    double gridTolNormX = -1.0, double gridTolNormY = -1.0);

// ── Tidy (RFC #4887 phase 5) ─────────────────────────────────────────────
//
// Resolve overlaps between movable items by minimal downward pushes,
// preserving every size and the left-to-right reading order.  Items named in
// `fixedIds` neither move nor count as overlap (the pan area: a meter over
// the spectrum is a feature, not disorder).  An item that cannot be pushed
// inside the surface without a new overlap is left exactly where it was —
// tidy never makes an arrangement worse.

struct TidyMove {
    QString  id;
    NormRect rect;
};

QList<TidyMove> tidyOverlaps(const QList<CanvasItem>& items,
                             const QStringList& fixedIds);

}  // namespace AetherSDR
