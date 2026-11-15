#pragma once

// "Classic": the shipping two-region shell (pan region split by one of the
// twelve layout ids, applet column beside it) as canvas placement (RFC #4887).
// It is what migration produces and what "reset to Classic" restores, so it must
// match the non-workspace UI exactly. Pure, so every id's geometry is pinned
// headless.

#include "gui/workspace/CanvasItem.h"
#include "gui/workspace/WorkspaceGeometry.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Fraction of the canvas width taken by the applet column in Classic.
//
// The real panel is a fixed-width widget (~260 px), not a fraction, so no
// single number is right at every window size; this approximates it at a
// typical 1440-1600 px window and is a starting point the operator can drag.
// Phase 3 can refine it from the panel's real width hint once the canvas
// actually hosts the panel.
inline constexpr double kClassicAppletColumnWidth = 0.18;

// Applets per Classic column before it wraps into another (slots stay above
// the 90 px display floor on a 1080 px canvas).  Classic caps at three
// columns; beyond that, applets compress within the last one.
inline constexpr int kClassicMaxAppletsPerColumn = 11;

// The panadapter cells for a layout id, in the order the stack assigns pans
// (A, B, C, ...), covering the whole unit square.
//
// Mirrors the kAllLayouts table in PanLayoutDialog.cpp — rows of equal-width
// cells, rows of equal height.  Returns an empty list for an unknown id
// rather than guessing: a caller that cannot place pans should fall back to
// single-pan, not invent a grid.
QList<NormRect> panCellsForLayout(const QString& layoutId);

// Every layout id this build understands, in the table's own order.
QStringList knownPanLayoutIds();

// How many pans a layout id describes (0 for an unknown id).
int panCountForLayout(const QString& layoutId);

// Compose the Classic arrangement. Pans beyond the layout's cell count are
// dropped; applets stack top to bottom in the column; an unknown/empty layoutId
// falls back to "1"; appletsLeft puts the column on the left. Ids are
// namespaced "pan:" / "applet:" so kinds never collide.
QList<CanvasItem> composeClassic(const QStringList& panIds,
                                 const QStringList& appletIds,
                                 const QString& layoutId,
                                 bool appletsLeft);

}  // namespace AetherSDR
