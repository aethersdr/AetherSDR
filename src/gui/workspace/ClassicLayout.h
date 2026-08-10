#pragma once

// "Classic" — today's two-region shell, expressed as canvas placement
// (RFC #4887, phase 2).
//
// The default workspace has to reproduce the application as it ships now: a
// panadapter region subdivided by one of the twelve hard-coded layout ids,
// and the applet column beside it.  An operator who never opens the workspace
// editor must see exactly what they see today, so this is the preset that
// migration produces and the one phase 6 offers as "reset to Classic".
//
// Pure: layout id in, normalized rects out.  No settings, no widgets, no
// radio — so every id's geometry is pinned headless, which matters because
// there are twelve of them and nobody is going to eyeball all twelve.

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

// Compose the Classic arrangement.
//
//   panIds      pans to place, in stack order; extras beyond the layout's
//               cell count are dropped (the layout is what the operator
//               chose, and inventing a thirteenth cell is worse than
//               honouring it).
//   appletIds   open applets, top to bottom, sharing the column.
//   layoutId    pan layout id; an unknown or empty id falls back to "1".
//   appletsLeft the column on the left instead of the right.
//
// Item ids are namespaced by kind ("pan:" / "applet:") so a pan and an applet
// can never collide in one CanvasLayout.
QList<CanvasItem> composeClassic(const QStringList& panIds,
                                 const QStringList& appletIds,
                                 const QString& layoutId,
                                 bool appletsLeft);

}  // namespace AetherSDR
