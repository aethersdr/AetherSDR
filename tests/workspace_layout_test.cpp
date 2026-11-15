// Unit tests for CanvasLayout — the widget-free rules of the workspace canvas
// (RFC #4887, phase 1).
//
// The invariant under test throughout is that z stays dense and contiguous
// over [0, count-1].  Sparse or duplicated z is the bug that makes "raise"
// quietly stop doing anything after enough operations, and it is invisible
// until an operator has been rearranging for a while — so it is asserted after
// every mutation here, not just at the end.

#include "gui/workspace/CanvasLayout.h"

#include <QPointF>
#include <QSize>
#include <QStringList>

#include <cmath>
#include <cstdio>

using AetherSDR::CanvasItem;
using AetherSDR::CanvasLayout;
using AetherSDR::NormRect;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

CanvasItem make(const QString& id, const NormRect& rect)
{
    CanvasItem it;
    it.id          = id;
    it.rect        = rect;
    it.minimumSize = QSize(0, 0);   // most cases are about placement, not floors
    return it;
}

// Every z value present exactly once, over [0, count-1].
bool zIsDense(const CanvasLayout& layout)
{
    const int n = layout.count();
    QList<bool> seen(n, false);
    for (const QString& id : layout.ids()) {
        const int z = layout.zOf(id);
        if (z < 0 || z >= n || seen[z]) {
            return false;
        }
        seen[z] = true;
    }
    return true;
}

QStringList zOrder(const CanvasLayout& layout)
{
    QStringList out;
    for (const CanvasItem& it : layout.itemsByZ()) {
        out.append(it.id);
    }
    return out;
}

}  // namespace

int main()
{
    // ── Membership ───────────────────────────────────────────────────────
    {
        CanvasLayout layout;

        report("empty layout has no items", layout.isEmpty() && layout.count() == 0);

        report("an item can be added",
               layout.addItem(make("a", NormRect{0.0, 0.0, 0.4, 0.4})));
        report("a duplicate id is refused",
               !layout.addItem(make("a", NormRect{0.5, 0.5, 0.2, 0.2})));
        report("an empty id is refused",
               !layout.addItem(make("", NormRect{0.5, 0.5, 0.2, 0.2})));
        report("the refusals did not change the layout", layout.count() == 1);

        report("contains() finds it",        layout.contains("a"));
        report("contains() misses a stranger", !layout.contains("nope"));
        report("item() returns the record",  layout.item("a") != nullptr);
        report("item() returns null for a stranger", layout.item("nope") == nullptr);

        report("removing an absent id fails", !layout.removeItem("nope"));
        report("removing a present id works", layout.removeItem("a"));
        report("layout is empty again",       layout.isEmpty());
    }

    // ── Insert clamps placement — bounds only, never growth ──────────────
    {
        CanvasLayout layout;
        CanvasItem it = make("wide", NormRect{0.8, 0.8, 0.5, 0.5});
        it.minimumSize = QSize(100, 100);
        layout.addItem(it);

        const NormRect stored = layout.item("wide")->rect;
        report("an item added off-surface slides back inside",
               std::fabs(stored.x - 0.5) < 1e-9 && std::fabs(stored.y - 0.5) < 1e-9);
        report("...keeping its size", std::fabs(stored.w - 0.5) < 1e-9);

        // The model clamp is canvas-independent BY DESIGN: minimum sizes are
        // a display concern.  Growing stored rects to meet a minimum against
        // whatever size the canvas happened to be is how a pre-layout replay
        // once rewrote an arrangement to full-canvas (phase 3 field report).
        CanvasItem tiny = make("tiny", NormRect{0.4, 0.4, 0.01, 0.01});
        tiny.minimumSize = QSize(400, 400);
        layout.addItem(tiny);
        report("a stored rect is never grown to meet a minimum",
               std::fabs(layout.item("tiny")->rect.w - 0.01) < 1e-9
                   && std::fabs(layout.item("tiny")->rect.h - 0.01) < 1e-9);
    }

    // ── z assignment and density ─────────────────────────────────────────
    {
        CanvasLayout layout;
        layout.addItem(make("a", NormRect{0.0, 0.0, 0.3, 0.3}));
        layout.addItem(make("b", NormRect{0.1, 0.1, 0.3, 0.3}));
        layout.addItem(make("c", NormRect{0.2, 0.2, 0.3, 0.3}));

        report("each new item lands on top",
               layout.zOf("a") == 0 && layout.zOf("b") == 1 && layout.zOf("c") == 2);
        report("z is dense after inserts", zIsDense(layout));
        report("ids() keeps insertion order",
               layout.ids() == QStringList({"a", "b", "c"}));
        report("itemsByZ() is bottom-to-top",
               zOrder(layout) == QStringList({"a", "b", "c"}));

        // Removing from the middle must close the hole, not leave z = {0, 2}.
        layout.removeItem("b");
        report("removal renumbers z", layout.zOf("a") == 0 && layout.zOf("c") == 1);
        report("z is dense after removal", zIsDense(layout));
    }

    // ── Rehydrating a saved stacking order ───────────────────────────────
    //
    // WorkspaceDocument persists z per item, and once an operator has raised
    // anything, z no longer matches array order.  Feeding stored items to
    // addItem() would silently restore the arrangement with its stacking
    // scrambled, so a restore path has to use restoreItems().
    {
        CanvasLayout layout;

        // A saved set whose z deliberately disagrees with array order —
        // exactly what "operator raised the bottom item" produces.
        CanvasItem a = make("a", NormRect{0.0, 0.0, 0.3, 0.3});
        CanvasItem b = make("b", NormRect{0.1, 0.1, 0.3, 0.3});
        CanvasItem c = make("c", NormRect{0.2, 0.2, 0.3, 0.3});
        a.z = 2;
        b.z = 0;
        c.z = 1;

        report("every stored item is restored",
               layout.restoreItems({a, b, c}) == 3);
        report("a restored arrangement keeps its saved stacking",
               zOrder(layout) == QStringList({"b", "c", "a"}));
        report("...and z is dense afterwards", zIsDense(layout));

        // Sparse saved z (an older document, or one hand-edited) restores in
        // the same relative order rather than being rejected.
        CanvasLayout sparse;
        CanvasItem x = make("x", NormRect{0.0, 0.0, 0.2, 0.2});
        CanvasItem y = make("y", NormRect{0.2, 0.2, 0.2, 0.2});
        x.z = 40;
        y.z = 10;
        sparse.restoreItems({x, y});
        report("sparse saved z is densified, preserving order",
               zOrder(sparse) == QStringList({"y", "x"}) && zIsDense(sparse));

        // Restore skips what it cannot place, and says how many it took.
        CanvasLayout guarded;
        CanvasItem dup1 = make("dup", NormRect{0.0, 0.0, 0.2, 0.2});
        CanvasItem dup2 = make("dup", NormRect{0.3, 0.3, 0.2, 0.2});
        CanvasItem anon = make("", NormRect{0.5, 0.5, 0.2, 0.2});
        report("restore skips duplicate and anonymous items",
               guarded.restoreItems({dup1, dup2, anon}) == 1
                   && guarded.count() == 1);

        // A plain add still puts a NEW item where the operator can see it,
        // whatever z it happens to carry.
        CanvasLayout fresh;
        CanvasItem buried = make("buried", NormRect{0.0, 0.0, 0.2, 0.2});
        buried.z = -99;
        fresh.addItem(make("first", NormRect{0.3, 0.3, 0.2, 0.2}));
        fresh.addItem(buried);
        report("addItem ignores the caller's z and lands on top",
               zOrder(fresh) == QStringList({"first", "buried"}));
    }

    // ── Stacking operations ──────────────────────────────────────────────
    {
        CanvasLayout layout;
        layout.addItem(make("a", NormRect{0.0, 0.0, 0.3, 0.3}));
        layout.addItem(make("b", NormRect{0.1, 0.1, 0.3, 0.3}));
        layout.addItem(make("c", NormRect{0.2, 0.2, 0.3, 0.3}));
        layout.addItem(make("d", NormRect{0.3, 0.3, 0.3, 0.3}));
        // bottom -> top: a b c d

        report("raise moves one step",
               layout.raise("a") && zOrder(layout) == QStringList({"b", "a", "c", "d"}));
        report("z is dense after raise", zIsDense(layout));

        report("lower moves one step back",
               layout.lower("a") && zOrder(layout) == QStringList({"a", "b", "c", "d"}));

        // Already at an extreme: nothing changes, and that is reported as
        // FALSE.  Callers turn these into change notifications, and under
        // auto-commit a notification is a whole-document write — so "already
        // frontmost" must not look like an edit.
        report("lower at the bottom reports no change",
               !layout.lower("a") && zOrder(layout) == QStringList({"a", "b", "c", "d"}));
        report("raise at the top reports no change",
               !layout.raise("d") && zOrder(layout) == QStringList({"a", "b", "c", "d"}));
        report("bringToFront on the frontmost reports no change",
               !layout.bringToFront("d") && zOrder(layout) == QStringList({"a", "b", "c", "d"}));
        report("sendToBack on the backmost reports no change",
               !layout.sendToBack("a") && zOrder(layout) == QStringList({"a", "b", "c", "d"}));
        report("z survives the no-ops", zIsDense(layout));

        report("bringToFront jumps to the top, others keep their order",
               layout.bringToFront("a")
                   && zOrder(layout) == QStringList({"b", "c", "d", "a"}));
        report("z is dense after bringToFront", zIsDense(layout));

        report("sendToBack jumps to the bottom, others keep their order",
               layout.sendToBack("d")
                   && zOrder(layout) == QStringList({"d", "b", "c", "a"}));
        report("z is dense after sendToBack", zIsDense(layout));

        report("stacking an unknown id fails",
               !layout.raise("nope") && !layout.lower("nope")
                   && !layout.bringToFront("nope") && !layout.sendToBack("nope"));
        report("zOf reports -1 for an unknown id", layout.zOf("nope") == -1);
    }

    // ── Hit testing ──────────────────────────────────────────────────────
    {
        CanvasLayout layout;
        // Two items that overlap in the middle of the canvas.
        layout.addItem(make("under", NormRect{0.10, 0.10, 0.40, 0.40}));
        layout.addItem(make("over",  NormRect{0.30, 0.30, 0.40, 0.40}));

        report("a point over only the lower item hits it",
               layout.hitTest(QPointF(0.15, 0.15)) == "under");
        report("a point over only the upper item hits it",
               layout.hitTest(QPointF(0.65, 0.65)) == "over");
        report("in the overlap, the topmost item wins",
               layout.hitTest(QPointF(0.35, 0.35)) == "over");

        // ...and it follows stacking, not insertion order.
        layout.bringToFront("under");
        report("the hit follows stacking after a raise",
               layout.hitTest(QPointF(0.35, 0.35)) == "under");

        report("bare canvas hits nothing",
               layout.hitTest(QPointF(0.95, 0.95)).isEmpty());
        report("outside the canvas hits nothing",
               layout.hitTest(QPointF(-0.5, 0.5)).isEmpty());
    }

    // ── setRect ──────────────────────────────────────────────────────────
    {
        CanvasLayout layout;
        layout.addItem(make("a", NormRect{0.0, 0.0, 0.4, 0.4}));

        report("setRect on an unknown id fails",
               !layout.setRect("nope", NormRect{0.0, 0.0, 0.2, 0.2}));

        report("setRect stores the new placement",
               layout.setRect("a", NormRect{0.5, 0.5, 0.25, 0.25})
                   && layout.item("a")->rect == NormRect{0.5, 0.5, 0.25, 0.25});

        report("setRect slides an off-surface request back inside",
               layout.setRect("a", NormRect{0.9, 0.9, 0.5, 0.5})
                   && layout.item("a")->rect == NormRect{0.5, 0.5, 0.5, 0.5});
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
