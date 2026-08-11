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
#include <QMouseEvent>
#include <QRect>
#include <QSignalSpy>
#include <QStringList>
#include <QWidget>

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

        // Both items fit at every size used above, so nothing was clamped and
        // nothing should have been announced as an edit.
        report("a resize that clamps nothing announces nothing",
               rectChanges == 0);

        // Shrinking below the default minimum DOES move things, and must say so.
        resizeAndSettle(&canvas, 100, 100);
        report("a resize that forces a clamp announces it", rectChanges > 0);
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

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
