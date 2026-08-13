// Offscreen tests for WorkspaceCanvas — the thin widget half of RFC #4887
// phase 1.
//
// CanvasLayout's rules are pinned in workspace_layout_test; what is checked
// here is only that the widget faithfully applies them to real geometry and
// real Qt stacking.  The two that matter most:
//
//   * takeItem() must hand a widget back ALIVE.  It is the call phase 3 needs
//     to move an applet between a canvas and a container, and a canvas that
//     destroys widgets on removal cannot be part of that path.
//   * a canvas resize must announce only the items that actually moved, or
//     phase 2's auto-commit would write the workspace document on every frame
//     of a window drag.

#include "gui/workspace/WorkspaceCanvas.h"

#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QTest>
#include <QRect>
#include <QSignalSpy>
#include <QStringList>
#include <QWidget>
#include <QWindow>

#include <cmath>
#include <cstdio>

using AetherSDR::NormRect;
using AetherSDR::WorkspaceCanvas;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

// Position of a child in its parent's child list.  Qt's stacking order IS
// this order — raise() moves a widget to the end — so it is how "on top" is
// observed without a screen.
bool nearly(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

int childIndex(const QWidget* parent, QWidget* child)
{
    return parent->children().indexOf(child);
}

void flushDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Resize and let the resize event actually land.  Qt may post rather than
// send QResizeEvent depending on platform and visibility, and the whole point
// of the resize cases is what the event handler does.
void resizeAndSettle(QWidget* w, int width, int height)
{
    w->resize(width, height);
    QCoreApplication::processEvents();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // ── Placement ────────────────────────────────────────────────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 800);
        canvas.show();

        auto* pan = new QWidget;
        report("an item can be placed",
               canvas.addItem("pan", pan, NormRect{0.0, 0.0, 0.72, 0.55}));
        report("the canvas adopted the widget", pan->parentWidget() == &canvas);
        report("the widget got the mapped pixels",
               pan->geometry() == QRect(0, 0, 720, 440));
        report("itemWidget() finds it",  canvas.itemWidget("pan") == pan);
        report("contains() finds it",    canvas.contains("pan"));
        report("itemCount() counts it",  canvas.itemCount() == 1);

        // A refused add must leave the caller's widget completely alone —
        // not reparent it, and above all not delete it.
        auto* orphan = new QWidget;
        report("a duplicate id is refused",
               !canvas.addItem("pan", orphan, NormRect{0.1, 0.1, 0.2, 0.2}));
        report("the refused widget is untouched",
               orphan->parentWidget() == nullptr);
        report("a null widget is refused",
               !canvas.addItem("other", nullptr, NormRect{0.1, 0.1, 0.2, 0.2}));
        report("an empty id is refused",
               !canvas.addItem("", orphan, NormRect{0.1, 0.1, 0.2, 0.2}));
        delete orphan;

        // Placement is clamped by the model, so what is read back is what is
        // stored, not what was asked for.
        canvas.setItemRect("pan", NormRect{0.9, 0.9, 0.5, 0.5});
        report("setItemRect clamps and applies",
               canvas.itemRect("pan") == NormRect{0.5, 0.5, 0.5, 0.5}
                   && pan->geometry() == QRect(500, 400, 500, 400));
        report("setItemRect on an unknown id fails",
               !canvas.setItemRect("nope", NormRect{0.0, 0.0, 0.2, 0.2}));
    }

    // ── Resize ───────────────────────────────────────────────────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        canvas.addItem("a", new QWidget, NormRect{0.0, 0.0, 0.5, 0.5});
        canvas.addItem("b", new QWidget, NormRect{0.5, 0.5, 0.5, 0.5});

        int rectChanges = 0;
        QObject::connect(&canvas, &WorkspaceCanvas::itemRectChanged,
                         [&rectChanges](const QString&, const NormRect&) {
                             ++rectChanges;
                         });

        resizeAndSettle(&canvas, 400, 200);
        report("children follow the canvas down",
               canvas.itemWidget("a")->geometry() == QRect(0, 0, 200, 100)
                   && canvas.itemWidget("b")->geometry() == QRect(200, 100, 200, 100));

        resizeAndSettle(&canvas, 1920, 1080);
        report("children follow the canvas back up",
               canvas.itemWidget("a")->geometry() == QRect(0, 0, 960, 540)
                   && canvas.itemWidget("b")->geometry() == QRect(960, 540, 960, 540));

        // A resize is never an edit.  Not when everything fits...
        report("a resize announces nothing", rectChanges == 0);

        // ...and not when the canvas is too small to honour minimums: the
        // DISPLAY compromises (the widget grows to stay usable) while the
        // stored rect stays pristine.  Persisting the compromise is how a
        // transient startup size once destroyed an arrangement for a whole
        // session (phase 3 field report).
        resizeAndSettle(&canvas, 100, 100);
        report("a squeezing resize still announces nothing", rectChanges == 0);
        report("...the display compromises",
               canvas.itemWidget("a")->geometry().width() > 50);
        report("...but the stored rect is untouched",
               canvas.itemRect("a") == NormRect{0.0, 0.0, 0.5, 0.5});

        // And the arrangement comes back the moment the canvas does.
        resizeAndSettle(&canvas, 1920, 1080);
        report("growing the canvas restores the exact arrangement",
               canvas.itemWidget("a")->geometry() == QRect(0, 0, 960, 540)
                   && canvas.itemWidget("b")->geometry() == QRect(960, 540, 960, 540));
    }

    // ── Stacking ─────────────────────────────────────────────────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* under = new QWidget;
        auto* over  = new QWidget;
        canvas.addItem("under", under, NormRect{0.1, 0.1, 0.4, 0.4});
        canvas.addItem("over",  over,  NormRect{0.3, 0.3, 0.4, 0.4});

        report("the later item stacks above",
               childIndex(&canvas, under) < childIndex(&canvas, over));

        report("bringItemToFront reorders real Qt stacking",
               canvas.bringItemToFront("under")
                   && childIndex(&canvas, over) < childIndex(&canvas, under));

        report("sendItemToBack reorders it back",
               canvas.sendItemToBack("under")
                   && childIndex(&canvas, under) < childIndex(&canvas, over));

        report("stacking an unknown id fails",
               !canvas.raiseItem("nope") && !canvas.bringItemToFront("nope"));
    }

    // ── Hit testing, in widget coordinates ───────────────────────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        canvas.addItem("under", new QWidget, NormRect{0.1, 0.1, 0.4, 0.4});
        canvas.addItem("over",  new QWidget, NormRect{0.3, 0.3, 0.4, 0.4});

        report("a pixel over one item resolves to it",
               canvas.hitTest(QPoint(150, 150)) == "under");
        report("a pixel in the overlap resolves to the topmost",
               canvas.hitTest(QPoint(350, 350)) == "over");
        report("a pixel over bare canvas resolves to nothing",
               canvas.hitTest(QPoint(950, 950)).isEmpty());
    }

    // ── Removal: take keeps, remove destroys ─────────────────────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* kept = new QWidget;
        canvas.addItem("kept", kept, NormRect{0.0, 0.0, 0.3, 0.3});

        QPointer<QWidget> keptGuard(kept);
        QWidget* handedBack = canvas.takeItem("kept");
        report("takeItem hands the same widget back", handedBack == kept);
        report("takeItem keeps it alive",             !keptGuard.isNull());
        report("takeItem detaches it",                kept->parentWidget() == nullptr);
        report("takeItem hides it",                   !kept->isVisible());
        report("takeItem drops it from the layout",   !canvas.contains("kept"));
        report("takeItem on an unknown id returns null",
               canvas.takeItem("nope") == nullptr);
        delete kept;

        auto* doomed = new QWidget;
        canvas.addItem("doomed", doomed, NormRect{0.0, 0.0, 0.3, 0.3});
        QPointer<QWidget> doomedGuard(doomed);
        report("removeItem reports success",  canvas.removeItem("doomed"));
        report("removeItem drops it from the layout", !canvas.contains("doomed"));
        flushDeferredDeletes();
        report("removeItem destroys the widget", doomedGuard.isNull());
        report("removeItem on an unknown id fails", !canvas.removeItem("nope"));
    }

    // ── Removal renumbers z, and the widget stacking follows ─────────────
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* a = new QWidget;
        auto* c = new QWidget;
        canvas.addItem("a", a,           NormRect{0.0, 0.0, 0.2, 0.2});
        canvas.addItem("b", new QWidget, NormRect{0.2, 0.2, 0.2, 0.2});
        canvas.addItem("c", c,           NormRect{0.4, 0.4, 0.2, 0.2});

        canvas.removeItem("b");
        flushDeferredDeletes();

        report("z closes up after a removal",
               canvas.layout().zOf("a") == 0 && canvas.layout().zOf("c") == 1);
        report("stacking survives the removal", childIndex(&canvas, a) < childIndex(&canvas, c));
    }

    // ── A widget destroyed behind the canvas's back leaves no ghost ──────
    //
    // Phase 3 moves applets between canvases and containers, which is exactly
    // where a widget can die out from under the model.  Without the
    // destroyed-watch the layout entry survives: contains() still reports it,
    // hitTest() still returns its id over dead space, and addItem() with that
    // id fails forever.
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* victim = new QWidget;
        canvas.addItem("victim", victim, NormRect{0.0, 0.0, 0.4, 0.4});
        canvas.addItem("bystander", new QWidget, NormRect{0.5, 0.5, 0.4, 0.4});

        QSignalSpy removedSpy(&canvas, &WorkspaceCanvas::itemRemoved);

        delete victim;   // not through removeItem() / takeItem()

        report("the model drops an externally destroyed item",
               !canvas.contains("victim"));
        report("...and the count follows",  canvas.itemCount() == 1);
        report("...and it is announced",    removedSpy.count() == 1);
        report("...and it no longer answers hit tests",
               canvas.hitTest(QPoint(100, 100)).isEmpty());
        report("...leaving the other item alone", canvas.contains("bystander"));

        // The id is reusable, which it would not be if the entry had survived.
        report("the freed id can be used again",
               canvas.addItem("victim", new QWidget, NormRect{0.0, 0.0, 0.3, 0.3}));

        // A widget handed back by takeItem() is no longer watched: destroying
        // it must not evict whatever holds its old id now.
        QWidget* taken = canvas.takeItem("bystander");
        canvas.addItem("bystander", new QWidget, NormRect{0.6, 0.6, 0.3, 0.3});
        delete taken;
        report("destroying a taken widget does not evict the id's new owner",
               canvas.contains("bystander"));
    }

    // ── A press raises, but a press on the frontmost is not an edit ──────
    //
    // eventFilter() runs bringItemToFront() on every press. Under auto-commit
    // every stacking notification is a whole-document write, so clicking the
    // item that is already on top must be silent — the same rule resizeEvent()
    // follows for a resize that clamps nothing.
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* under = new QWidget;
        auto* over  = new QWidget;
        canvas.addItem("under", under, NormRect{0.1, 0.1, 0.4, 0.4});
        canvas.addItem("over",  over,  NormRect{0.5, 0.5, 0.4, 0.4});

        QSignalSpy stackSpy(&canvas, &WorkspaceCanvas::itemStackingChanged);

        const auto press = [](QWidget* w) {
            QMouseEvent ev(QEvent::MouseButtonPress, QPointF(5, 5),
                           w->mapToGlobal(QPointF(5, 5)),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &ev);
        };

        // "over" is already frontmost — pressing it changes nothing.
        press(over);
        report("pressing the frontmost item emits nothing", stackSpy.count() == 0);
        report("...and it is still frontmost",
               childIndex(&canvas, under) < childIndex(&canvas, over));

        // Pressing the one underneath is a real change.
        press(under);
        report("pressing a lower item raises it", stackSpy.count() == 1);
        report("...and Qt stacking followed",
               childIndex(&canvas, over) < childIndex(&canvas, under));

        // Pressing it again is now a no-op.
        press(under);
        report("pressing it again emits nothing", stackSpy.count() == 1);
    }

    // ── Restoring a saved surface through the widget ─────────────────────
    //
    // addItem() ignores a caller's z by design, and this is the only public
    // way onto a canvas — so without restoreItems() a phase-3 restore would
    // scramble stacking unless it happened to feed items in ascending z.
    {
        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        // Saved order deliberately disagrees with array order.
        AetherSDR::CanvasItem a;
        a.id   = QStringLiteral("a");
        a.rect = NormRect{0.0, 0.0, 0.3, 0.3};
        a.z    = 2;
        AetherSDR::CanvasItem b;
        b.id   = QStringLiteral("b");
        b.rect = NormRect{0.1, 0.1, 0.3, 0.3};
        b.z    = 0;
        AetherSDR::CanvasItem c;
        c.id   = QStringLiteral("c");
        c.rect = NormRect{0.2, 0.2, 0.3, 0.3};
        c.z    = 1;

        auto* wa = new QWidget;
        auto* wb = new QWidget;
        auto* wc = new QWidget;
        const QHash<QString, QWidget*> widgets{{"a", wa}, {"b", wb}, {"c", wc}};

        report("every item with a widget is placed",
               canvas.restoreItems({a, b, c}, widgets) == 3);
        report("the saved stacking is restored, not array order",
               canvas.layout().zOf("b") == 0 && canvas.layout().zOf("c") == 1
                   && canvas.layout().zOf("a") == 2);
        report("...and real Qt stacking matches",
               childIndex(&canvas, wb) < childIndex(&canvas, wc)
                   && childIndex(&canvas, wc) < childIndex(&canvas, wa));
        report("geometry came from the stored rects",
               wa->geometry() == QRect(0, 0, 300, 300));

        // An item with no widget is skipped rather than guessed at.
        AetherSDR::CanvasItem orphan;
        orphan.id   = QStringLiteral("ghost");
        orphan.rect = NormRect{0.5, 0.5, 0.2, 0.2};
        report("an item with no widget is skipped",
               canvas.restoreItems({orphan}, {}) == 0
                   && !canvas.contains("ghost"));
    }

    // ── Selection + the gesture session (phase 5) ────────────────────────
    {
        using AetherSDR::HitZone;

        WorkspaceCanvas canvas;
        canvas.resize(1000, 1000);
        canvas.show();

        auto* a = new QWidget;
        canvas.addItem("a", a, NormRect{0.1, 0.1, 0.4, 0.4});
        canvas.addItem("b", new QWidget, NormRect{0.6, 0.6, 0.3, 0.3});

        // Grid snap off for this scenario: it pins the session mechanics
        // (follow, cancel, drag-out, keyboard), and on a 1000 px test canvas
        // the 96-division grid has a line within the magnet of nearly every
        // hand-picked rect.  The grid tier has its own case below.
        canvas.setGridSnapEnabled(false);

        QSignalSpy selSpy(&canvas, &WorkspaceCanvas::selectionChanged);
        QSignalSpy startSpy(&canvas, &WorkspaceCanvas::gestureStarted);
        QSignalSpy finishSpy(&canvas, &WorkspaceCanvas::gestureFinished);
        QSignalSpy outSpy(&canvas, &WorkspaceCanvas::itemDraggedOut);

        // Pressing an item selects it (through the real event filter).
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5),
                          a->mapToGlobal(QPointF(5, 5)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(a, &press);
        report("pressing an item selects it",
               canvas.selectedItem() == "a" && selSpy.count() == 1);

        // A move gesture: begin at a global point, move right+down, end.
        const QPoint origin = canvas.mapToGlobal(QPoint(500, 500));
        canvas.beginMoveGesture("a", origin);
        report("the gesture reports its start rect",
               startSpy.count() == 1
                   && qvariant_cast<NormRect>(startSpy.at(0).at(1))
                          == NormRect{0.1, 0.1, 0.4, 0.4});
        canvas.moveGesture(origin + QPoint(100, 50));   // +0.1, +0.05
        report("the item follows live",
               canvas.itemRect("a") == NormRect{0.2, 0.15, 0.4, 0.4});
        canvas.endGesture(origin + QPoint(100, 50));
        report("release commits and announces",
               finishSpy.count() == 1 && canvas.itemRect("a") == NormRect{0.2, 0.15, 0.4, 0.4});

        // Esc mid-gesture restores the start rect and never announces.
        canvas.beginMoveGesture("a", origin);
        canvas.moveGesture(origin + QPoint(300, 300));
        canvas.cancelGesture();
        report("cancel restores the start rect",
               canvas.itemRect("a") == NormRect{0.2, 0.15, 0.4, 0.4});
        report("...and does not report a finish", finishSpy.count() == 1);

        // A move released OUTSIDE the canvas restores the rect and reports
        // the drag-out with the release point.
        canvas.beginMoveGesture("a", origin);
        canvas.moveGesture(origin + QPoint(200, 0));
        canvas.endGesture(canvas.mapToGlobal(QPoint(2000, 500)));
        report("a release outside restores the rect",
               canvas.itemRect("a") == NormRect{0.2, 0.15, 0.4, 0.4});
        report("...and reports the drag-out once",
               outSpy.count() == 1 && finishSpy.count() == 1);

        // Resize through the session: E grip, anchor pinned.
        canvas.selectItem("a");
        canvas.beginFrameGesture(HitZone::E, origin);
        canvas.moveGesture(origin + QPoint(100, 0));
        canvas.endGesture(origin + QPoint(100, 0));
        report("a frame resize grows the gripped edge",
               canvas.itemRect("a") == NormRect{0.2, 0.15, 0.5, 0.4});

        // Snapping in a live move: b's left edge is at 0.6; drag a's right
        // edge to 0.594 — within the 8 px tolerance — and it captures.
        canvas.beginMoveGesture("a", origin);
        canvas.moveGesture(origin + QPoint(-106, 0));
        canvas.endGesture(origin + QPoint(-106, 0));
        report("a live move snaps to a peer edge",
               nearly(canvas.itemRect("a").right(), 0.6));

        // The grid tier through a live gesture: from x=0.1, +213 px puts the
        // origin at 0.313 — no peer line in reach, but 30/96 = 0.3125 is
        // 0.0005 away, inside the 4 px grid magnet — the move lands ON the
        // grid line.
        canvas.setGridSnapEnabled(true);
        canvas.beginMoveGesture("a", origin);
        canvas.moveGesture(origin + QPoint(213, 0));
        canvas.endGesture(origin + QPoint(213, 0));
        report("a live move lands on the snap grid",
               nearly(canvas.itemRect("a").x, 30.0 / 96.0));
        canvas.setGridSnapEnabled(false);

        // A PAN ignores applet magnets and reaches the grid (8600 field
        // report): with an applet layered on it whose left edge (324 px) is
        // 3 px from the drop point, the pan must pass it by and land on the
        // grid line at 31/96 (322.9 px, 1.9 px away) — peer capture would
        // have beaten the grid for any other item type.  The applet keeps
        // its pan magnetism: dropped 2.9 px from the pan's right edge, it
        // captures the edge even though a grid line sits further inside
        // its own magnet range.
        {
            canvas.setGridSnapEnabled(true);
            auto* pan = new QWidget;
            canvas.addItem("pan:0", pan, NormRect{0.20, 0.50, 0.30, 0.30},
                           QStringLiteral("panadapter"));
            canvas.addItem("applet:A", new QWidget,
                           NormRect{0.324, 0.50, 0.20, 0.25});

            canvas.beginMoveGesture("pan:0", origin);
            canvas.moveGesture(origin + QPoint(121, 0));
            canvas.endGesture(origin + QPoint(121, 0));
            report("a pan passes applet edges and snaps to the grid",
                   nearly(canvas.itemRect("pan:0").x, 31.0 / 96.0)
                       && nearly(canvas.itemRect("pan:0").y, 0.5));

            const double panRight = canvas.itemRect("pan:0").x
                                    + canvas.itemRect("pan:0").w;
            canvas.beginMoveGesture("applet:A", origin);
            canvas.moveGesture(origin + QPoint(296, 0));
            canvas.endGesture(origin + QPoint(296, 0));
            report("an applet still snaps to the pan's edge",
                   nearly(canvas.itemRect("applet:A").x, panRight));

            canvas.removeItem("pan:0");
            canvas.removeItem("applet:A");
            canvas.setGridSnapEnabled(false);
        }

        // ── Red-team gaps: the locked posture under REAL events ──────────
        // No prior test ever pressed a locked canvas — the bare widget
        // defaults to editing and every press fixture inherited it (B2/m6/
        // m7 were invisible to the whole suite).
        {
            canvas.addItem("locked:x", new QWidget, NormRect{0.1, 0.6, 0.2, 0.2});
            canvas.addItem("locked:y", new QWidget, NormRect{0.15, 0.65, 0.2, 0.2});
            const int zBefore = canvas.layout().zOf("locked:x");
            canvas.setEditMode(false);
            report("leaving edit clears the selection",
                   canvas.selectedItem().isEmpty());

            // A press that lands on the canvas (an item's dead space
            // propagating) must not select, raise, or persist anything.
            QMouseEvent press(QEvent::MouseButtonPress,
                              QPointF(150, 650),
                              canvas.mapToGlobal(QPointF(150, 650)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&canvas, &press);
            report("a locked canvas ignores presses (B2)",
                   canvas.selectedItem().isEmpty()
                       && canvas.layout().zOf("locked:x") == zBefore);

            canvas.setEditMode(true);
            QMouseEvent mid(QEvent::MouseButtonPress,
                            QPointF(150, 650),
                            canvas.mapToGlobal(QPointF(150, 650)),
                            Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&canvas, &mid);
            report("middle-click is not placement (m7)",
                   canvas.selectedItem().isEmpty());

            canvas.removeItem("locked:x");
            canvas.removeItem("locked:y");
        }

        // ── Red-team M1: hit-test what the operator SEES ─────────────────
        // Stored below the 160x90 display floor: the widget is drawn
        // min-clamped, and a click inside the visible margin must select
        // THE item, not whatever the stored rect leaves behind it.
        {
            canvas.addItem("tiny", new QWidget, NormRect{0.30, 0.30, 0.02, 0.02});
            // stored: 300..320 x 300..320 on the 1000px canvas; display:
            // 300..460 x 300..390 (160x90 floor).
            report("a click in the clamp margin hits the visible item (M1)",
                   canvas.hitTest(QPoint(440, 370)) == QStringLiteral("tiny"));
            report("...and past the display edge misses it",
                   canvas.hitTest(QPoint(470, 400)) != QStringLiteral("tiny"));
            canvas.removeItem("tiny");
        }

        // ── Red-team M2: the write boundary refuses hostile rects ────────
        {
            canvas.addItem("guard", new QWidget, NormRect{0.5, 0.5, 0.2, 0.2});
            const NormRect held = canvas.itemRect("guard");
            report("setItemRect refuses a zero-size rect",
                   !canvas.setItemRect("guard", NormRect{0.1, 0.1, 0.0, 0.0})
                       && canvas.itemRect("guard") == held);
            report("...and a non-finite one",
                   !canvas.setItemRect("guard",
                                       NormRect{std::nan(""), 0.1, 0.2, 0.2})
                       && canvas.itemRect("guard") == held);
            canvas.removeItem("guard");
        }

        // Keyboard: arrows nudge, Shift+arrows resize, both announce a
        // gesture pair (undo hangs off it).
        canvas.selectItem("a");
        const int startCount = startSpy.count();
        const NormRect before = canvas.itemRect("a");
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &right);
        report("an arrow nudges by the step",
               nearly(canvas.itemRect("a").x, before.x + 8.0 / 1000.0));
        QKeyEvent grow(QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&canvas, &grow);
        report("Shift+arrow resizes the SE corner",
               nearly(canvas.itemRect("a").h, before.h + 8.0 / 1000.0)
                   && nearly(canvas.itemRect("a").y, before.y));
        report("keyboard placement announces gesture pairs",
               startSpy.count() == startCount + 2
                   && finishSpy.count() >= 3);

        // Esc with no gesture clears the selection.
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &esc);
        report("Esc clears the selection",
               canvas.selectedItem().isEmpty());
    }

    // ── Placement keys beat application shortcuts — only while selected ──
    //
    // The main window binds Left/Right and Shift+Left/Right to frequency
    // nudges.  The field report: Shift+arrows only resized vertically,
    // because the horizontal pair was eaten by those shortcuts before the
    // canvas saw them.  Pinned with a real competing QShortcut and keys
    // delivered through the full chain (QTest::keyClick consults the
    // shortcut map; sendEvent would bypass it and prove nothing).
    {
        QWidget window;
        window.resize(1000, 1000);
        auto* canvas = new WorkspaceCanvas;
        canvas->setParent(&window);
        canvas->setGeometry(0, 0, 1000, 1000);
        window.show();
        canvas->show();

        int nudges = 0;
        auto* freqNudge = new QShortcut(
            QKeySequence(Qt::SHIFT | Qt::Key_Right), &window);
        // ApplicationShortcut: WindowShortcut needs an ACTIVE window, which
        // the offscreen platform never grants.  Same QShortcutMap, same
        // ShortcutOverride consultation — the mechanism under test.
        freqNudge->setContext(Qt::ApplicationShortcut);
        QObject::connect(freqNudge, &QShortcut::activated,
                         [&nudges] { ++nudges; });

        canvas->addItem("a", new QWidget, NormRect{0.1, 0.1, 0.4, 0.4});
        canvas->setFocus();

        // Keys must travel the REAL delivery chain — window system event →
        // shortcut map → ShortcutOverride → focus widget.  The QWidget
        // overload of keyClick sends straight to the widget and consults no
        // shortcut map, which would pass here while proving nothing.
        QWindow* handle = window.windowHandle();

        // No selection: the shortcut owns the key, the item is untouched.
        QTest::keyClick(handle, Qt::Key_Right, Qt::ShiftModifier);
        report("without a selection the app shortcut fires", nudges == 1);
        report("...and the item is untouched",
               canvas->itemRect("a") == NormRect{0.1, 0.1, 0.4, 0.4});

        // Selected: the canvas reclaims the key — Shift+Right resizes
        // HORIZONTALLY, the axis the field report lost.
        canvas->selectItem("a");
        QTest::keyClick(handle, Qt::Key_Right, Qt::ShiftModifier);
        report("with a selection Shift+Right resizes width",
               nearly(canvas->itemRect("a").w, 0.4 + 8.0 / 1000.0));
        report("...and the app shortcut did not fire", nudges == 1);

        // Both axes under one modifier — no Shift/Ctrl+Shift axis split.
        QTest::keyClick(handle, Qt::Key_Down, Qt::ShiftModifier);
        report("Shift+Down resizes height under the same modifier",
               nearly(canvas->itemRect("a").h, 0.4 + 8.0 / 1000.0));

        // Plain arrows move while selected (they were also bound app-wide).
        auto* plainNudge = new QShortcut(QKeySequence(Qt::Key_Left), &window);
        plainNudge->setContext(Qt::ApplicationShortcut);
        int plain = 0;
        QObject::connect(plainNudge, &QShortcut::activated,
                         [&plain] { ++plain; });
        QTest::keyClick(handle, Qt::Key_Left, Qt::NoModifier);
        report("plain arrows move the selected item, not the VFO",
               plain == 0 && nearly(canvas->itemRect("a").x, 0.1 - 8.0 / 1000.0));

        // Deselect: the keys go straight back to the shortcuts.
        canvas->clearSelection();
        QTest::keyClick(handle, Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyClick(handle, Qt::Key_Left, Qt::NoModifier);
        report("deselecting returns the keys to the app",
               nudges == 2 && plain == 1);
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
