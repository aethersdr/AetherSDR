#pragma once

// The set of items on one canvas surface, and the rules that keep it coherent
// (RFC #4887, phase 1).
//
// Widget-free by design.  Everything a canvas does that can be got wrong —
// clamping placement, deciding what is under the cursor, keeping stacking
// order sane — lives here so it is testable headless, leaving WorkspaceCanvas
// as a thin widget that only applies the answers.  See
// tests/workspace_layout_test.cpp.
//
// Item counts are in the tens (26 applets + up to 8 pans), so lookup is a
// linear scan over a QList.  A hash keyed by id would win nothing measurable
// and would cost the stable insertion order that makes the tests readable.

#include "gui/workspace/CanvasItem.h"

#include <QList>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>

namespace AetherSDR {

class CanvasLayout {
public:
    // ── Membership ───────────────────────────────────────────────────────
    //
    // Rejects an empty or duplicate id: identity is what every other call
    // here takes as an argument, so letting a second "applet:RX" in would
    // make every later lookup ambiguous rather than merely wrong.  The item's
    // z is assigned on insert (topmost) and its rect clamped, so a caller
    // cannot introduce an off-canvas or buried item by construction.
    bool addItem(CanvasItem item, const QSize& canvas);
    bool removeItem(const QString& id);
    void clear();

    bool contains(const QString& id) const;
    const CanvasItem* item(const QString& id) const;
    int count() const { return static_cast<int>(m_items.size()); }
    bool isEmpty() const { return m_items.isEmpty(); }

    // Insertion order — stable, and independent of stacking.
    QStringList ids() const;

    // Bottom-to-top.  This is the order to apply stacking in, and the reverse
    // of the order to hit-test in.
    QList<CanvasItem> itemsByZ() const;

    // ── Placement ────────────────────────────────────────────────────────
    //
    // The rect is clamped against the canvas before it is stored, so the
    // layout never holds a placement the operator cannot see.  Returns false
    // for an unknown id only — a clamped-away rect is still a successful set.
    bool setRect(const QString& id, const NormRect& rect, const QSize& canvas);

    // Re-clamp every item, for when the canvas itself changed size.  Returns
    // the ids whose rects actually moved, so a caller can persist just those
    // (phase 2) rather than rewriting the whole document on every resize.
    QStringList reclampAll(const QSize& canvas);

    // ── Hit testing ──────────────────────────────────────────────────────
    //
    // Topmost item containing the point, in canvas fractions; empty when the
    // point is over bare canvas.  Overlap is legal (items layer), so "topmost"
    // is the whole answer: the highest z wins, which is what the operator sees
    // and therefore what they expect to click.
    QString hitTest(const QPointF& normPoint) const;

    // ── Stacking ─────────────────────────────────────────────────────────
    //
    // z values stay dense and contiguous over [0, count-1] after every one of
    // these.  Sparse or duplicated z is the bug that makes "raise" stop doing
    // anything after enough operations, so the invariant is restored eagerly
    // rather than checked for.
    bool raise(const QString& id);          // one step toward the front
    bool lower(const QString& id);          // one step toward the back
    bool bringToFront(const QString& id);
    bool sendToBack(const QString& id);

    int zOf(const QString& id) const;       // -1 when absent

private:
    CanvasItem* find(const QString& id);
    const CanvasItem* find(const QString& id) const;

    // Reassign z over [0, count-1] preserving current relative order, with
    // insertion order as the tie-break so the result is deterministic.
    void normalizeZ();

    QList<CanvasItem> m_items;   // insertion order
};

}  // namespace AetherSDR
