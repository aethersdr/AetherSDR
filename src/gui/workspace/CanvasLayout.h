#pragma once

// The items on one canvas surface and the rules that keep it coherent (RFC
// #4887): clamping, hit-testing and stacking, widget-free so they are tested
// headless (tests/workspace_layout_test.cpp); WorkspaceCanvas only applies the
// answers. Item counts are in the tens, so a linear QList scan keeps stable
// insertion order at no measurable cost.

#include "gui/workspace/CanvasItem.h"

#include <QList>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>

namespace AetherSDR {

class CanvasLayout {
public:
    // Rejects an empty or duplicate id. Assigns topmost z and clamps the rect with
    // clampToBounds() (canvas-independent; minimum size is enforced at display time
    // in WorkspaceCanvas, since a model-side minimum corrupts rects placed before
    // layout). For saved arrangements use restoreItems(), which keeps stored z.
    bool addItem(CanvasItem item);

    // Rehydrate a saved surface: sorts by stored z, inserts bottom-to-top, and
    // densifies once at the end. Batch-only because densifying per insert would
    // lose the relative order of stored z values. Skips empty/duplicate ids;
    // returns how many were inserted.
    int restoreItems(const QList<CanvasItem>& items);
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
    // The rect is bounds-clamped before it is stored, so the layout never
    // holds a placement outside the unit square.  Returns false for an
    // unknown id only — a clamped-away rect is still a successful set.
    bool setRect(const QString& id, const NormRect& rect);

    // ── Hit testing ──────────────────────────────────────────────────────
    //
    // Topmost item containing the point, in canvas fractions; empty when the
    // point is over bare canvas.  Overlap is legal (items layer), so "topmost"
    // is the whole answer: the highest z wins, which is what the operator sees
    // and therefore what they expect to click.
    QString hitTest(const QPointF& normPoint) const;

    // z stays dense over [0, count-1] after every call (sparse/duplicate z makes
    // raise stop working). Each returns true only if stacking changed (false for an
    // unknown id or an item already at that end), because callers emit a change
    // that auto-commits the document. Use contains() to tell missing from no-op.
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
