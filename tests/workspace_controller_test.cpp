// WorkspaceController — canvas mode end to end (RFC #4887 phase 3), against
// a real store, canvas and container manager in a temporary home.
//
// The contract under test is the membership rule stated in the controller's
// header: a document item means "belongs on the canvas"; placement requires
// mode-on + open + not-floating.  The cases that matter most:
//
//   * an explicit return FORGETS the canvas home (no bounce-back through the
//     dockModeChanged hook), while closing an applet KEEPS it;
//   * first enable migrates the legacy keys and starts dual-write — and a
//     store refusing (newer-schema write block, PR #4900 H1) fails the
//     enable cleanly instead of moving a single widget;
//   * disable returns every widget yet keeps every item, so re-enable
//     restores the arrangement.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/containers/ContainerManager.h"
#include "gui/containers/ContainerWidget.h"
#include "gui/workspace/WorkspaceCanvas.h"
#include "gui/workspace/WorkspaceController.h"
#include "gui/workspace/ClassicLayout.h"
#include "gui/workspace/WorkspaceStore.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QSet>
#include <QMimeData>
#include <QRect>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <iostream>

using AetherSDR::AppSettings;
using AetherSDR::CanvasItem;
using AetherSDR::ContainerManager;
using AetherSDR::ContainerWidget;
using AetherSDR::NormRect;
using AetherSDR::Workspace;
using AetherSDR::WorkspaceCanvas;
using AetherSDR::WorkspaceController;
using AetherSDR::WorkspaceDocument;
using AetherSDR::WorkspaceStore;
using AetherSDR::WorkspaceSurface;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

bool nearly(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

QString storedDocument()
{
    QString value;
    if (!AppSettings::instance().readStationRowFromDisk(
            WorkspaceStore::kSettingsKey, value)) {
        return {};
    }
    return value;
}

// A drop of the applet MIME at a canvas position, exactly as Qt would
// deliver it — this exercises the canvas's real drop path, not a shortcut
// into the controller.
void dropOnCanvas(WorkspaceCanvas* canvas, const QString& payload, QPointF norm)
{
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-aethersdr-applet"),
                 payload.toUtf8());
    const QPointF pos(norm.x() * canvas->width(), norm.y() * canvas->height());

    QDragEnterEvent enter(pos.toPoint(), Qt::MoveAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &enter);
    QDropEvent drop(pos, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &drop);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-workspace-controller-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();

    // Legacy keys the first enable migrates from.
    AppSettings::instance().setValue(QStringLiteral("Applet_RX"), QStringLiteral("True"));
    AppSettings::instance().setValue(QStringLiteral("Applet_TX"), QStringLiteral("True"));
    AppSettings::instance().setValue(QStringLiteral("AppletOrder"), QStringLiteral("RX,TX"));

    // The world: a stand-in panel, two applet containers, a canvas, and the
    // pan stack widget the reserved item hosts.
    QWidget panel;
    auto* panelLayout = new QVBoxLayout(&panel);
    ContainerManager mgr(&panel);

    auto* rx = mgr.createContainer("RX", "RX Controls");
    auto* tx = mgr.createContainer("TX", "TX Controls");
    rx->setContent(new QLabel("rx"));
    tx->setContent(new QLabel("tx"));
    panelLayout->addWidget(rx);
    panelLayout->addWidget(tx);
    rx->setContainerVisible(true);
    tx->setContainerVisible(true);
    panel.show();

    WorkspaceCanvas canvas;
    canvas.resize(1000, 800);
    canvas.show();

    // A stand-in pan host (phase 4): one live pan as a plain QWidget, hooks
    // over little lambdas — the same seam MainWindow_Workspace wires over
    // the real PanadapterStack, with none of its GPU baggage.
    QWidget panOwner;
    auto* panBox = new QVBoxLayout(&panOwner);
    QMap<QString, QWidget*> pans;
    QSet<QString> floatingPans;
    auto* panA = new QWidget(&panOwner);
    panBox->addWidget(panA);
    pans.insert(QStringLiteral("0x40000000"), panA);
    panOwner.show();

    WorkspaceController ctl(&mgr, &canvas);
    WorkspaceController::PanHostHooks hooks;
    hooks.panIds = [&] { return QStringList(pans.keys()); };
    hooks.detach = [&](const QString& id) -> QWidget* {
        return floatingPans.contains(id) ? nullptr : pans.value(id, nullptr);
    };
    hooks.restore = [&](const QString& id, QWidget* w) {
        Q_UNUSED(id);
        panBox->addWidget(w);
        w->show();
    };
    hooks.isFloating = [&](const QString& id) {
        return floatingPans.contains(id);
    };
    ctl.setPanHost(hooks);
    // The recall close-loop is scoped to the widget catalog (red-team B1),
    // so the switch matrix needs it from the start.
    ctl.setWidgetCatalog({{QStringLiteral("RX"), QStringLiteral("RX Controls"),
                           QStringLiteral("Receive")},
                          {QStringLiteral("TX"), QStringLiteral("TX Controls"),
                           QStringLiteral("Transmit")}});

    // ── Boot before any document exists ──────────────────────────────────
    report("boot with nothing stored asks for nothing", !ctl.boot());
    report("...and wrote no document", storedDocument().isEmpty());

    // ── First enable: migrate + place ────────────────────────────────────
    {
        QString whyNot;
        report("enable succeeds", ctl.enable({"RX", "TX"}, &whyNot));
        report("...and reports enabled", ctl.isEnabled());
        report("the FIRST enable (migration) opens editing",
               canvas.isEditMode());

        report("both applets are on the canvas",
               rx->isOnCanvas() && tx->isOnCanvas()
                   && canvas.contains("applet:RX") && canvas.contains("applet:TX"));
        report("...and left the panel layout",
               panelLayout->indexOf(rx) < 0 && panelLayout->indexOf(tx) < 0);
        report("the live pan rides as its slot item (phase 4)",
               canvas.contains(QStringLiteral("pan:0"))
                   && panA->parentWidget() == &canvas);
        report("the pan sits behind the applets",
               canvas.layout().zOf(QStringLiteral("pan:0")) == 0);

        const QString doc = storedDocument();
        report("the document was flushed with the mode on",
               doc.contains(QStringLiteral("canvasEnabled"))
                   && doc.contains(QStringLiteral("pan:0")));
        report("the legacy keys survive (dual-write)",
               AppSettings::instance().contains(QStringLiteral("Applet_RX")));

        report("enable twice is a no-op", ctl.enable({"RX", "TX"}));
    }

    // ── Explicit return forgets the home ─────────────────────────────────
    {
        report("an applet returns to the panel", ctl.returnAppletToPanel("RX"));
        report("...panel-docked, in its old slot",
               rx->isPanelDocked() && panelLayout->indexOf(rx) == 0);
        report("...off the canvas", !canvas.contains("applet:RX"));
        report("...and its home is forgotten",
               !storedDocument().contains(QStringLiteral("applet:RX")));
        report("returning it again fails", !ctl.returnAppletToPanel("RX"));

        report("sending it back works", ctl.sendAppletToCanvas("RX"));
        report("...and the home is recorded again",
               storedDocument().contains(QStringLiteral("applet:RX")));
    }

    // ── Closing keeps the home; reopening returns to it ──────────────────
    {
        const NormRect before = canvas.itemRect("applet:TX");

        tx->setContainerVisible(false);
        report("a closed applet leaves the canvas",
               !canvas.contains("applet:TX") && tx->isPanelDocked());
        report("...but keeps its home",
               storedDocument().contains(QStringLiteral("applet:TX")));

        tx->setContainerVisible(true);
        report("reopening returns it to the canvas", tx->isOnCanvas());
        report("...at the rect it had",
               canvas.itemRect("applet:TX") == before);
    }

    // ── Drops: move an item, place a panel applet ────────────────────────
    {
        dropOnCanvas(&canvas, "applet-move-check", QPointF(0.5, 0.5));
        report("a drop naming nothing does nothing",
               canvas.itemCount() == 3);

        const NormRect before = canvas.itemRect("applet:RX");
        dropOnCanvas(&canvas, "RX", QPointF(0.4, 0.6));
        const NormRect after = canvas.itemRect("applet:RX");
        report("dropping a canvas item moves it",
               after.w == before.w && after.h == before.h
                   && !(after == before));
        report("...centred on the drop point",
               qAbs((after.x + after.w / 2.0) - 0.4) < 0.01
                   && qAbs((after.y + after.h / 2.0) - 0.6) < 0.01);
        report("...and the move is persisted",
               storedDocument().contains(QStringLiteral("applet:RX")));

        ctl.returnAppletToPanel("RX");
        dropOnCanvas(&canvas, "RX", QPointF(0.3, 0.3));
        report("dropping a panel applet places it",
               rx->isOnCanvas() && canvas.contains("applet:RX"));
    }

    // ── Float from the canvas: evicted, home forgotten ───────────────────
    {
        mgr.floatContainer("RX");
        report("a canvas applet can float", rx->isFloating());
        report("...leaving the canvas", !canvas.contains("applet:RX"));
        report("...and forgetting its home",
               !storedDocument().contains(QStringLiteral("applet:RX")));

        mgr.dockContainer("RX");
        report("docking after a canvas float lands in the panel, not the canvas",
               rx->isPanelDocked() && !rx->isOnCanvas());
    }

    // ── Temporary disable: relaunch still restores canvas ────────────────
    {
        ctl.disable(WorkspaceController::DisablePersistence::PreserveEnabled);
        report("temporary disable reports disabled", !ctl.isEnabled());
        report("...but preserves the enabled preference on disk",
               storedDocument().contains(QStringLiteral("canvasEnabled")));
        {
            QWidget restartPanel;
            ContainerManager restartManager(&restartPanel);
            WorkspaceCanvas restartCanvas;
            WorkspaceController restartProbe(&restartManager, &restartCanvas);
            report("...so a fresh controller restores canvas on relaunch",
                   restartProbe.boot());
        }
        report("re-enable after a temporary handoff succeeds",
               ctl.enable({"RX", "TX"}));
    }

    // ── Explicit disable: everything returns, placement is kept ──────────
    {
        ctl.disable();
        report("disable reports disabled", !ctl.isEnabled());
        report("every applet is back in the panel",
               rx->isPanelDocked() && tx->isPanelDocked()
                   && panelLayout->indexOf(tx) >= 0);
        report("the pan went back to its owner, one-step (never nullptr)",
               panA->parentWidget() == &panOwner
                   && !canvas.contains(QStringLiteral("pan:0")));
        report("the canvas is empty", canvas.itemCount() == 0);

        const QString doc = storedDocument();
        report("the mode is off on disk",
               !doc.contains(QStringLiteral("canvasEnabled")));
        report("...but placement is kept for re-enable",
               doc.contains(QStringLiteral("applet:TX")));

        report("re-enable places from the kept document",
               ctl.enable({"RX", "TX"}) && tx->isOnCanvas());
        report("...and comes up LOCKED (operating posture)",
               !canvas.isEditMode());
        report("...and boot on a fresh controller now asks for the mode",
               [&] {
                   WorkspaceStore probe;
                   return probe.load() && probe.document().canvasEnabled;
               }());
        ctl.disable();
    }

    // ── Phase 5: undo, tidy, reset, drag-out return ──────────────────────
    {
        // RX forgot its canvas home in the float block above, so re-enable
        // places only TX; send RX back explicitly.
        report("re-enable for the phase 5 block",
               ctl.enable({"RX", "TX"}) && tx->isOnCanvas()
                   && ctl.sendAppletToCanvas("RX") && rx->isOnCanvas());

        // Locked first: a gesture must be refused outright (the edit-mode
        // field request — interacting with an applet must not arm
        // placement), then editing is entered for the rest of the block.
        {
            const NormRect held = canvas.itemRect("applet:RX");
            canvas.beginMoveGesture("applet:RX",
                                    canvas.mapToGlobal(QPoint(500, 400)));
            report("a locked canvas arms no gesture",
                   !canvas.gestureActive()
                       && canvas.itemRect("applet:RX") == held);
        }
        canvas.setEditMode(true);

        // The offscreen platform stacks every top-level at the same origin,
        // which would make "over the panel" also "inside the canvas".  Pull
        // them apart so global points are unambiguous.
        canvas.window()->move(0, 0);
        panel.window()->move(3000, 0);

        // Undo restores the rect the last gesture started from; twice
        // toggles (single-slot redo).
        const NormRect before = canvas.itemRect("applet:RX");
        const QPoint origin = canvas.mapToGlobal(QPoint(500, 400));
        canvas.beginMoveGesture("applet:RX", origin);
        canvas.moveGesture(origin + QPoint(120, 80));
        canvas.endGesture(origin + QPoint(120, 80));
        const NormRect after = canvas.itemRect("applet:RX");
        report("a live gesture moved the item", !(after == before));

        report("undo restores the pre-gesture rect",
               ctl.undoLastPlacement()
                   && canvas.itemRect("applet:RX") == before);
        report("undo again toggles back (redo)",
               ctl.undoLastPlacement()
                   && canvas.itemRect("applet:RX") == after);

        // Tidy: overlap TX onto RX, then resolve — TX pushed clear, sizes
        // kept; panstack overlaps ignored throughout.
        const NormRect rxRect = canvas.itemRect("applet:RX");
        NormRect overlap = rxRect;
        overlap.y += rxRect.h / 2.0;   // half-covering RX
        canvas.setItemRect("applet:TX", overlap);
        ctl.tidyLayout();
        const NormRect txTidied = canvas.itemRect("applet:TX");
        report("tidy pushes the overlapping applet clear",
               txTidied.y >= rxRect.bottom() - 1e-9
                   && nearly(txTidied.h, overlap.h));

        // Reset to Classic: fresh column derived from the live legacy keys.
        canvas.setItemRect("applet:RX", NormRect{0.05, 0.05, 0.3, 0.3});
        ctl.resetToClassic();
        report("reset places the applets back in the Classic column",
               nearly(canvas.itemRect("applet:RX").x,
                      1.0 - AetherSDR::kClassicAppletColumnWidth)
                   && rx->isOnCanvas() && tx->isOnCanvas());
        report("...and the pan slot takes the remainder",
               nearly(canvas.itemRect(QStringLiteral("pan:0")).w,
                      1.0 - AetherSDR::kClassicAppletColumnWidth));

        // Drag-out over the panel returns the applet to it; a drag-out over
        // nothing is an abort the canvas already restored.
        ctl.setReturnTarget(&panel);
        const QPoint overPanel = panel.mapToGlobal(QPoint(10, 10));
        canvas.beginMoveGesture("applet:RX", origin);
        canvas.moveGesture(origin + QPoint(50, 0));
        canvas.endGesture(overPanel);
        report("a release over the panel returns the applet",
               rx->isPanelDocked() && !canvas.contains("applet:RX"));

        const NormRect txBefore = canvas.itemRect("applet:TX");
        canvas.beginMoveGesture("applet:TX", origin);
        canvas.moveGesture(origin + QPoint(50, 0));
        canvas.endGesture(canvas.mapToGlobal(QPoint(-500, -500)));
        report("a release over nothing restores and stays",
               tx->isOnCanvas() && canvas.itemRect("applet:TX") == txBefore);

        ctl.sendAppletToCanvas("RX");
        ctl.disable();
    }

    // ── Phase 4: pan lifecycle through the hooks seam ────────────────────
    {
        report("re-enable for the phase 4 block",
               ctl.enable({"RX", "TX"}) && canvas.contains("pan:0"));
        canvas.setEditMode(true);   // the block below arranges

        // A second pan arrives (radio granted `pan create`).
        auto* panB = new QWidget(&panOwner);
        pans.insert(QStringLiteral("0x40000001"), panB);
        ctl.onPanAdded(QStringLiteral("0x40000001"));
        report("a new pan is placed as the next slot",
               canvas.contains("pan:1") && panB->parentWidget() == &canvas);
        report("...and its slot is persisted",
               storedDocument().contains(QStringLiteral("pan:1")));
        report("...at the BACK — never over the operator's arrangement "
               "(the 8600 field report)",
               canvas.layout().zOf(QStringLiteral("pan:1"))
                       < canvas.layout().zOf(QStringLiteral("applet:RX"))
                   && canvas.layout().zOf(QStringLiteral("pan:1"))
                          < canvas.layout().zOf(QStringLiteral("applet:TX")));

        // Float: the applet has been adopted elsewhere (here: simulated by
        // the flag); the entry is released, the document item survives.
        const NormRect homeB = canvas.itemRect("pan:1");
        floatingPans.insert(QStringLiteral("0x40000001"));
        ctl.onPanFloated(QStringLiteral("0x40000001"));
        report("a floated pan leaves the canvas",
               !canvas.contains("pan:1"));
        report("...keeping its document home",
               storedDocument().contains(QStringLiteral("pan:1")));

        // Dock: back to the very spot.
        floatingPans.remove(QStringLiteral("0x40000001"));
        ctl.onPanDocked(QStringLiteral("0x40000001"));
        report("docking returns the pan to its canvas home",
               canvas.contains("pan:1") && canvas.itemRect("pan:1") == homeB);

        // Rekey (FLEX band recall): same applet, new id, slot follows.
        ctl.onPanRekeyed(QStringLiteral("0x40000001"),
                         QStringLiteral("0x40000005"));
        pans.insert(QStringLiteral("0x40000005"),
                    pans.take(QStringLiteral("0x40000001")));
        report("a rekeyed pan keeps its slot item",
               ctl.panItemIdFor(QStringLiteral("0x40000005"))
                       == QStringLiteral("pan:1")
                   && canvas.contains("pan:1"));

        // Remove: entry released pre-destruction, slot freed for reuse,
        // document item kept.
        ctl.onPanRemoved(QStringLiteral("0x40000005"));
        pans.remove(QStringLiteral("0x40000005"));
        report("a removed pan releases its entry",
               !canvas.contains("pan:1"));
        report("...but the document keeps the slot",
               storedDocument().contains(QStringLiteral("pan:1")));

        auto* panC = new QWidget(&panOwner);
        pans.insert(QStringLiteral("0x40000002"), panC);
        ctl.onPanAdded(QStringLiteral("0x40000002"));
        report("the freed slot is reused, at its remembered rect",
               canvas.contains("pan:1") && canvas.itemRect("pan:1") == homeB);

        // Two pans with NO stored slot rect must not stack pixel-identical
        // (red-team B3): defaults cascade, and a deliberate new pan sits
        // above the older pans while staying below every applet.
        {
            auto* panD = new QWidget(&panOwner);
            auto* panE = new QWidget(&panOwner);
            pans.insert(QStringLiteral("0x40000010"), panD);
            pans.insert(QStringLiteral("0x40000011"), panE);
            // Fresh slots -> no document rect for either.
            ctl.onPanAdded(QStringLiteral("0x40000010"));
            ctl.onPanAdded(QStringLiteral("0x40000011"));
            const QString dId = ctl.panItemIdFor(QStringLiteral("0x40000010"));
            const QString eId = ctl.panItemIdFor(QStringLiteral("0x40000011"));
            report("defaulted pans cascade to distinct rects (B3)",
                   canvas.itemRect(dId).x != canvas.itemRect(eId).x);
            report("...the newer sits above the older, below the applets",
                   canvas.layout().zOf(eId) > canvas.layout().zOf(dId)
                       && canvas.layout().zOf(eId)
                              < canvas.layout().zOf(QStringLiteral("applet:RX")));
            ctl.onPanRemoved(QStringLiteral("0x40000010"));
            ctl.onPanRemoved(QStringLiteral("0x40000011"));
            pans.remove(QStringLiteral("0x40000010"));
            pans.remove(QStringLiteral("0x40000011"));
        }

        // Band stack: hosted as its own item while the mode is on.
        QWidget bandStack(&panOwner);
        hooks.bandStack        = [&]() -> QWidget* { return &bandStack; };
        hooks.reclaimBandStack = [&](QWidget* w) {
            w->setParent(&panOwner);
        };
        ctl.setPanHost(hooks);
        ctl.setBandStackVisible(true);
        report("the band stack becomes a canvas item",
               canvas.contains(WorkspaceController::kBandStackItemId)
                   && bandStack.parentWidget() == &canvas);
        ctl.setBandStackVisible(false);
        report("...and hiding re-homes it",
               !canvas.contains(WorkspaceController::kBandStackItemId)
                   && bandStack.parentWidget() == &panOwner
                   && !bandStack.isVisible());
        // The hooks hold a pointer to this block-local panel, and the M1
        // band-stack recall invokes them on every later switch — detach
        // before the widget dies (the same lifetime rule as the guard).
        hooks.bandStack        = {};
        hooks.reclaimBandStack = {};
        ctl.setPanHost(hooks);

        // A reclaimed widget cannot hide behind a healthy model: steal the
        // pan back to its owner behind the canvas's back (what the stack's
        // rebuild paths did in the 8600 field report), and the next
        // placement request must heal — drop the stale entry, re-detach,
        // re-place — rather than trust contains().
        panBox->addWidget(pans.value(QStringLiteral("0x40000002")));   // theft
        report("the model still claims the stolen pan",
               canvas.contains("pan:1"));
        ctl.onPanDocked(QStringLiteral("0x40000002"));
        report("a placement request heals a stolen pan",
               canvas.contains("pan:1")
                   && pans.value(QStringLiteral("0x40000002"))->parentWidget()
                          == &canvas);

        // A z-poisoned document cannot resurrect a pan over the controls:
        // force the pan frontmost (recording it), cycle the mode, and the
        // replay must normalize pans back below everything (8600 report).
        canvas.bringItemToFront(QStringLiteral("pan:1"));
        report("poison recorded: the pan sits over the applets",
               canvas.layout().zOf(QStringLiteral("pan:1"))
                   > canvas.layout().zOf(QStringLiteral("applet:RX")));
        ctl.disable();
        report("re-enable normalizes pans back to the bottom",
               ctl.enable({"RX", "TX"})
                   && canvas.layout().zOf(QStringLiteral("pan:1"))
                          < canvas.layout().zOf(QStringLiteral("applet:RX"))
                   && canvas.layout().zOf(QStringLiteral("pan:1"))
                          < canvas.layout().zOf(QStringLiteral("applet:TX")));

        // A layout-selector change while canvas mode owns the pans must
        // replace the stale stored pan rectangles, not merely rearrange the
        // hidden legacy splitter. It changes only active-main live pans:
        // applets and extra surfaces remain the operator arranged them.
        auto* panD2 = new QWidget(&panOwner);
        auto* panE2 = new QWidget(&panOwner);
        pans.insert(QStringLiteral("0x40000010"), panD2);
        pans.insert(QStringLiteral("0x40000011"), panE2);
        ctl.onPanAdded(QStringLiteral("0x40000010"));
        ctl.onPanAdded(QStringLiteral("0x40000011"));
        const QString pan0 = ctl.panItemIdFor(QStringLiteral("0x40000000"));
        const QString pan1 = ctl.panItemIdFor(QStringLiteral("0x40000002"));
        const QString pan2 = ctl.panItemIdFor(QStringLiteral("0x40000010"));
        const QString pan3 = ctl.panItemIdFor(QStringLiteral("0x40000011"));
        const QString preservedSurface =
            ctl.addCanvasWindow(QStringLiteral("Pan layout preservation"));
        const NormRect appletBefore = canvas.itemRect(QStringLiteral("applet:RX"));
        canvas.setItemRect(pan0, NormRect{0.30, 0.30, 0.60, 0.60});
        canvas.setItemRect(pan1, NormRect{0.30, 0.30, 0.60, 0.60});
        canvas.setItemRect(pan2, NormRect{0.30, 0.30, 0.60, 0.60});
        canvas.setItemRect(pan3, NormRect{0.30, 0.30, 0.60, 0.60});
        ctl.commitPlacement();

        AppSettings::instance().setValue(QStringLiteral("Applet_RX"), QStringLiteral("True"));
        AppSettings::instance().setValue(QStringLiteral("Applet_TX"), QStringLiteral("True"));
        AppSettings::instance().setValue(QStringLiteral("AppletPanelVisible"),
                                         QStringLiteral("True"));
        AppSettings::instance().setValue(QStringLiteral("AppletPanelDockedLeft"),
                                         QStringLiteral("False"));
        AppSettings::instance().setValue(QStringLiteral("PanadapterLayout"),
                                         QStringLiteral("4v"));
        AppSettings::instance().save();
        report("canvas pan layout accepts canonical 4v",
               ctl.applyPanLayout(QStringLiteral("4v")));
        const double classicPanWidth = 1.0 - AetherSDR::kClassicAppletColumnWidth;
        report("4v replaces stale pan rectangles with canonical cells",
               nearly(canvas.itemRect(pan0).x, 0.0)
                   && nearly(canvas.itemRect(pan0).y, 0.0)
                   && nearly(canvas.itemRect(pan0).w, classicPanWidth)
                   && nearly(canvas.itemRect(pan0).h, 0.25)
                   && nearly(canvas.itemRect(pan1).y, 0.25)
                   && nearly(canvas.itemRect(pan2).y, 0.50)
                   && nearly(canvas.itemRect(pan3).y, 0.75));
        report("canvas pan layout preserves applet and extra surface",
               canvas.itemRect(QStringLiteral("applet:RX")) == appletBefore
                   && !preservedSurface.isEmpty()
                   && storedDocument().contains(preservedSurface));

        // A lower-slot pop-out stays untouched and does not leave a blank
        // canonical cell between the active main-surface pans.
        floatingPans.insert(QStringLiteral("0x40000002"));
        ctl.onPanFloated(QStringLiteral("0x40000002"));
        report("floating pan is outside active-main layout membership",
               !ctl.activeMainPanIdsForLayout().contains(
                   QStringLiteral("0x40000002")));
        AppSettings::instance().setValue(QStringLiteral("PanadapterLayout"),
                                         QStringLiteral("3v"));
        AppSettings::instance().save();
        report("floating lower slot does not consume a canvas layout cell",
               ctl.applyPanLayout(QStringLiteral("3v"))
                   && !canvas.contains(pan1)
                   && nearly(canvas.itemRect(pan0).y, 0.0)
                   && nearly(canvas.itemRect(pan2).y, 1.0 / 3.0)
                   && nearly(canvas.itemRect(pan3).y, 2.0 / 3.0));
        floatingPans.remove(QStringLiteral("0x40000002"));
        ctl.onPanDocked(QStringLiteral("0x40000002"));

        AppSettings::instance().setValue(QStringLiteral("PanadapterLayout"),
                                         QStringLiteral("2x2"));
        AppSettings::instance().save();
        report("2x2 maps live pan slots in Classic order",
               ctl.applyPanLayout(QStringLiteral("2x2"))
                   && nearly(canvas.itemRect(pan0).x, 0.0)
                   && nearly(canvas.itemRect(pan0).y, 0.0)
                   && nearly(canvas.itemRect(pan0).w, classicPanWidth / 2.0)
                   && nearly(canvas.itemRect(pan0).h, 0.5)
                   && nearly(canvas.itemRect(pan1).x, classicPanWidth / 2.0)
                   && nearly(canvas.itemRect(pan1).y, 0.0)
                   && nearly(canvas.itemRect(pan2).x, 0.0)
                   && nearly(canvas.itemRect(pan2).y, 0.5)
                   && nearly(canvas.itemRect(pan3).x, classicPanWidth / 2.0)
                   && nearly(canvas.itemRect(pan3).y, 0.5));
        WorkspaceDocument persisted;
        const bool parsedStored = WorkspaceDocument::fromStoredJson(
            storedDocument().toUtf8(), &persisted);
        const Workspace* persistedWs = parsedStored
            ? persisted.workspace(persisted.activeWorkspace) : nullptr;
        const WorkspaceSurface* persistedMain = persistedWs
            ? persistedWs->surface(WorkspaceSurface::kMainId) : nullptr;
        const CanvasItem* persistedPan3 = nullptr;
        if (persistedMain) {
            for (const CanvasItem& item : persistedMain->items) {
                if (item.id == pan3) {
                    persistedPan3 = &item;
                    break;
                }
            }
        }
        report("2x2 canonical geometry reached stored workspace JSON",
               persistedPan3 && nearly(persistedPan3->rect.x, classicPanWidth / 2.0)
                   && nearly(persistedPan3->rect.y, 0.5)
                   && nearly(persistedPan3->rect.w, classicPanWidth / 2.0)
                   && nearly(persistedPan3->rect.h, 0.5));

        // A shrink can remove a pan whose radio id sorts last even though its
        // stable canvas slot is in the middle. Re-apply after that actual
        // removal: surviving non-contiguous slots must fill all 3v cells.
        ctl.onPanRekeyed(QStringLiteral("0x40000010"),
                         QStringLiteral("0xf0000010"));
        pans.insert(QStringLiteral("0xf0000010"),
                    pans.take(QStringLiteral("0x40000010")));
        ctl.onPanRemoved(QStringLiteral("0xf0000010"));
        pans.remove(QStringLiteral("0xf0000010"));
        report("post-rekey shrink reflows the actual surviving slots",
               ctl.applyPanLayout(QStringLiteral("3v"))
                   && nearly(canvas.itemRect(pan0).y, 0.0)
                   && nearly(canvas.itemRect(pan1).y, 1.0 / 3.0)
                   && nearly(canvas.itemRect(pan3).y, 2.0 / 3.0));

        // Restore a fourth live pan for the surrounding lifecycle cleanup;
        // the freed stable slot is reused rather than minting a fifth item.
        auto* replacementPanD2 = new QWidget(&panOwner);
        pans.insert(QStringLiteral("0x40000012"), replacementPanD2);
        ctl.onPanAdded(QStringLiteral("0x40000012"));
        report("replacement pan reuses the removed stable slot",
               ctl.panItemIdFor(QStringLiteral("0x40000012")) == pan2);
        const QString beforeInvalid = storedDocument();
        report("invalid canvas pan layout is a no-op",
               !ctl.applyPanLayout(QStringLiteral("not-a-layout"))
                   && storedDocument() == beforeInvalid);
        ctl.removeCanvasWindow(preservedSurface);
        ctl.onPanRemoved(QStringLiteral("0x40000012"));
        ctl.onPanRemoved(QStringLiteral("0x40000011"));
        pans.remove(QStringLiteral("0x40000012"));
        pans.remove(QStringLiteral("0x40000011"));

        ctl.onPanRemoved(QStringLiteral("0x40000002"));
        pans.remove(QStringLiteral("0x40000002"));
        ctl.disable();
        const QString beforeDisabled = storedDocument();
        report("disabled canvas pan layout is a no-op",
               !ctl.applyPanLayout(QStringLiteral("4v"))
                   && storedDocument() == beforeDisabled);
    }

    // ── Phase 6: workspaces — CRUD, full-recall switching, bindings ──────
    {
        report("re-enable for the phase 6 block",
               ctl.enable({"RX", "TX"}) && canvas.contains("pan:0"));
        canvas.setEditMode(true);
        rx->setContainerVisible(true);
        tx->setContainerVisible(true);
        if (!rx->isOnCanvas()) ctl.sendAppletToCanvas("RX");
        if (!tx->isOnCanvas()) ctl.sendAppletToCanvas("TX");
        const QString wsA = ctl.activeWorkspaceId();

        // Create-from-current duplicates and switches to the copy.
        const QString wsB = ctl.createWorkspace(
            WorkspaceController::NewWorkspaceSource::Current,
            QStringLiteral("Contest"));
        report("create-from-current switches to the copy",
               !wsB.isEmpty() && ctl.activeWorkspaceId() == wsB
                   && canvas.contains("applet:RX") && canvas.contains("applet:TX"));
        report("...and a fresh canvas opens EDITING (field request)",
               canvas.isEditMode());
        canvas.setEditMode(false);
        report("...while a plain switch keeps the posture",
               ctl.switchWorkspace(wsA) && !canvas.isEditMode()
                   && ctl.switchWorkspace(wsB));
        canvas.setEditMode(true);
        report("...and the switcher lists both",
               ctl.workspaceList().size() >= 2);

        // Close TX here: the workspace remembers it as closed.
        tx->setContainerVisible(false);
        report("closing writes the closed flag to the active workspace",
               storedDocument().contains(QStringLiteral("closed")));

        // Switch back to A: FULL RECALL reopens TX (open there).
        report("switching restores the other workspace's open set",
               ctl.switchWorkspace(wsA)
                   && tx->isContainerVisible() && tx->isOnCanvas()
                   && rx->isOnCanvas());

        // Switch to B again: TX closes (closed there), RX stays.
        report("...and the closed set on the way back",
               ctl.switchWorkspace(wsB)
                   && !tx->isContainerVisible()
                   && rx->isOnCanvas());
        report("a switch is a whole-surface change: undo is cleared",
               !ctl.canUndo());

        // Pans: rects are per-workspace.
        const NormRect inB = canvas.itemRect("pan:0");
        ctl.switchWorkspace(wsA);
        canvas.setItemRect("pan:0", NormRect{0.05, 0.05, 0.4, 0.4});
        ctl.switchWorkspace(wsB);
        report("pan rects are per-workspace",
               canvas.itemRect("pan:0") == inB);
        ctl.switchWorkspace(wsA);
        report("...both ways",
               canvas.itemRect("pan:0") == NormRect{0.05, 0.05, 0.4, 0.4});

        // Blank starts empty; live pans land at cascade defaults.
        const QString wsC = ctl.createWorkspace(
            WorkspaceController::NewWorkspaceSource::Blank,
            QStringLiteral("Bare"));
        report("blank workspace: pans placed, no applets",
               ctl.activeWorkspaceId() == wsC
                   && canvas.contains("pan:0")
                   && !canvas.contains("applet:RX")
                   && !rx->isContainerVisible());

        // Bindings: a bound global profile recall switches; tx-type and
        // unbound recalls do not (decisions 6/8).
        ctl.bindProfile(QStringLiteral("CW Heavy"), wsB);
        ctl.onRadioProfileLoaded(QStringLiteral("tx"), QStringLiteral("CW Heavy"));
        report("a tx-profile recall never switches",
               ctl.activeWorkspaceId() == wsC);
        ctl.onRadioProfileLoaded(QStringLiteral("global"),
                                 QStringLiteral("CW Heavy"));
        report("a bound global recall switches",
               ctl.activeWorkspaceId() == wsB);
        ctl.onRadioProfileLoaded(QStringLiteral("global"),
                                 QStringLiteral("Unbound Profile"));
        report("an unbound recall leaves the workspace alone (decision 8)",
               ctl.activeWorkspaceId() == wsB);

        // Delete the active workspace: real switch to the fallback.
        report("deleting the active workspace falls back and re-places",
               ctl.deleteWorkspace(wsB)
                   && ctl.activeWorkspaceId() != wsB
                   && canvas.contains("pan:0"));
        report("rename reflects in the list",
               ctl.renameWorkspace(wsC, QStringLiteral("Renamed"))
                   && [&] {
                          for (const auto& p : ctl.workspaceList())
                              if (p.second == QStringLiteral("Renamed"))
                                  return true;
                          return false;
                      }());

        // Red-team B1: a container OUTSIDE the catalog (composite children,
        // the sidebar) is never the recall's business — switches must not
        // touch it.
        {
            auto* inner = mgr.createContainer("gate", "Gate (composite child)");
            inner->setContent(new QLabel("g"));
            inner->setContainerVisible(true);
            ctl.switchWorkspace(wsA);
            ctl.switchWorkspace(ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Current,
                QStringLiteral("B1Probe")));
            report("a non-catalog container survives switches (B1)",
                   inner->isContainerVisible());
            ctl.switchWorkspace(wsA);
            report("...both ways", inner->isContainerVisible());
        }

        // Red-team B2: recall-driven bulk visibility runs inside the
        // recall guard, so the panel can suppress preference writes.
        {
            int on = 0, off = 0;
            bool balanced = true, inOrder = true;
            hooks.recallGuard = [&](bool g) {
                if (g) { ++on; if (on != off + 1) inOrder = false; }
                else   { ++off; if (off != on) balanced = false; }
            };
            ctl.setPanHost(hooks);
            const QString probe = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Blank,
                QStringLiteral("B2Probe"));
            ctl.switchWorkspace(wsA);
            report("recall brackets the guard, balanced and ordered (B2)",
                   on >= 2 && balanced && inOrder);
            ctl.deleteWorkspace(probe);
            // The guard captured this block's locals by reference and the
            // controller keeps the hooks copy — detach it before they die.
            hooks.recallGuard = {};
            ctl.setPanHost(hooks);
        }

        // Blank workspaces cascade multiple pans (agent review): two live
        // pans with no slot items must land at distinct rects.
        {
            auto* panB2 = new QWidget(&panOwner);
            pans.insert(QStringLiteral("0x40000020"), panB2);
            ctl.onPanAdded(QStringLiteral("0x40000020"));
            const QString blank2 = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Blank,
                QStringLiteral("TwoPanBlank"));
            const QString id0 = ctl.panItemIdFor(QStringLiteral("0x40000000"));
            const QString id1 = ctl.panItemIdFor(QStringLiteral("0x40000020"));
            report("a blank workspace cascades multiple pans",
                   canvas.contains(id0) && canvas.contains(id1)
                       && canvas.itemRect(id0).x != canvas.itemRect(id1).x);
            ctl.deleteWorkspace(blank2);
            ctl.onPanRemoved(QStringLiteral("0x40000020"));
            pans.remove(QStringLiteral("0x40000020"));
            ctl.switchWorkspace(wsA);
        }

        // m9: the store NEVER holds a dangling activeWorkspace, even
        // mid-delete of the active one.
        {
            const QString doomed = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Current,
                QStringLiteral("Doomed"));
            report("delete-active keeps the stored active valid (m9)",
                   ctl.deleteWorkspace(doomed)
                       && !storedDocument().contains(QStringLiteral("Doomed"))
                       && storedDocument().contains(ctl.activeWorkspaceId()));
        }

        // m8: a closed pop-out must not desert workspaces that list it —
        // a hidden float participates in recall (reopens as a float).
        {
            ctl.switchWorkspace(wsA);
            canvas.setEditMode(true);
            rx->setContainerVisible(true);
            if (!rx->isOnCanvas()) ctl.sendAppletToCanvas("RX");
            const QString twin = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Current,
                QStringLiteral("M8Twin"));   // lists RX open
            ctl.switchWorkspace(wsA);
            mgr.floatContainer("RX");        // wsA forgets; the twin keeps it
            rx->setContainerVisible(false);  // operator closes the pop-out
            report("m8 precondition: hidden float",
                   rx->isFloating() && !rx->isContainerVisible());
            ctl.switchWorkspace(twin);
            report("a workspace that lists a closed pop-out reopens it (m8)",
                   rx->isContainerVisible());
            ctl.deleteWorkspace(twin);
            if (rx->isFloating()) mgr.dockContainer("RX");
            rx->setContainerVisible(true);
            ctl.switchWorkspace(wsA);
            ctl.sendAppletToCanvas("RX");
        }

        // K6OZY: the boot/replay closed-flag paths — a closed flag skips a
        // shut applet, and an applet reopened while the mode was OFF beats
        // its stale flag (cleared and placed on the next enable).
        {
            ctl.switchWorkspace(wsA);
            canvas.setEditMode(true);
            tx->setContainerVisible(true);
            if (!tx->isOnCanvas()) ctl.sendAppletToCanvas("TX");
            tx->setContainerVisible(false);   // closed -> flag written
            ctl.disable();
            report("closed flag survives disable",
                   storedDocument().contains(QStringLiteral("closed")));

            // Case A: still closed -> enable skips it.
            ctl.enable({"RX", "TX"});
            report("a closed-flagged applet is not placed at enable",
                   !canvas.contains("applet:TX") && !tx->isContainerVisible());

            // Case B: reopened while the mode was OFF -> the click wins.
            ctl.disable();
            tx->setContainerVisible(true);    // hook is inert while disabled
            ctl.enable({"RX", "TX"});
            report("an applet reopened while disabled beats its stale flag",
                   canvas.contains("applet:TX") && tx->isOnCanvas());
            canvas.setEditMode(true);
        }

        // K6OZY: reset composes Classic on the ACTUAL sparse slots — no
        // global renumber orphaning other workspaces' pan items.
        {
            auto* panS = new QWidget(&panOwner);
            pans.insert(QStringLiteral("0x40000030"), panS);
            ctl.onPanAdded(QStringLiteral("0x40000030"));   // slot 1
            const QString twin = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Current,
                QStringLiteral("SlotTwin"));
            const QString slot1 = ctl.panItemIdFor(QStringLiteral("0x40000030"));
            canvas.setEditMode(true);
            canvas.setItemRect(slot1, NormRect{0.61, 0.11, 0.35, 0.35});
            ctl.commitPlacement();
            ctl.switchWorkspace(wsA);
            // Free slot 0: the map goes sparse {1}.
            ctl.onPanRemoved(QStringLiteral("0x40000000"));
            pans.remove(QStringLiteral("0x40000000"));
            ctl.resetToClassic();
            ctl.switchWorkspace(twin);
            report("reset does not renumber slots under other workspaces",
                   canvas.itemRect(slot1) == NormRect{0.61, 0.11, 0.35, 0.35});
            // restore the world
            ctl.switchWorkspace(wsA);
            ctl.deleteWorkspace(twin);
            ctl.onPanRemoved(QStringLiteral("0x40000030"));
            pans.remove(QStringLiteral("0x40000030"));
            auto* panA2 = new QWidget(&panOwner);
            pans.insert(QStringLiteral("0x40000000"), panA2);
            ctl.onPanAdded(QStringLiteral("0x40000000"));
        }

        // Import pop-outs (phase 6): a floated applet and a floated pan
        // land on the canvas at rects mapped from their window geometry.
        {
            ctl.switchWorkspace(wsA);
            canvas.setEditMode(true);
            mgr.floatContainer("RX");
            report("import precondition: RX floats", rx->isFloating());

            floatingPans.insert(QStringLiteral("0x40000000"));
            ctl.onPanFloated(QStringLiteral("0x40000000"));
            QRect fakePanWin(canvas.mapToGlobal(QPoint(100, 100)),
                             QSize(400, 300));
            hooks.floatingPanGlobalRect = [fakePanWin](const QString&) {
                return fakePanWin;
            };
            hooks.requestDock = [&](const QString& id) {
                floatingPans.remove(id);
                ctl.onPanDocked(id);
            };
            ctl.setPanHost(hooks);

            const int n = ctl.importFloatingOntoCanvas();
            report("import lands both pop-outs",
                   n == 2 && rx->isOnCanvas() && !rx->isFloating()
                       && canvas.contains(ctl.panItemIdFor(
                              QStringLiteral("0x40000000"))));
            const NormRect panRect = canvas.itemRect(
                ctl.panItemIdFor(QStringLiteral("0x40000000")));
            // Canvas is 1000x800: 100px -> 0.1/0.125, 400x300 -> 0.4/0.375.
            report("...the pan at its window's mapped rect",
                   nearly(panRect.x, 0.1) && nearly(panRect.y, 0.125)
                       && nearly(panRect.w, 0.4) && nearly(panRect.h, 0.375));
            report("...and nothing left floating to import",
                   ctl.importFloatingOntoCanvas() == 0);

            // m7: an oversized float (maximized on a bigger monitor) must
            // not bury the canvas as a full-surface item.
            floatingPans.insert(QStringLiteral("0x40000000"));
            ctl.onPanFloated(QStringLiteral("0x40000000"));
            hooks.floatingPanGlobalRect = [&](const QString&) {
                return QRect(canvas.mapToGlobal(QPoint(-200, -200)),
                             QSize(4000, 3000));
            };
            ctl.setPanHost(hooks);
            report("an oversized float imports capped below full-surface (m7)",
                   ctl.importFloatingOntoCanvas() == 1
                       && canvas.itemRect(ctl.panItemIdFor(
                              QStringLiteral("0x40000000"))).w <= 0.9 + 1e-9);
        }

        // The widget palette (Add widget ▸ engine).
        {
            ctl.switchWorkspace(wsA);
            canvas.setEditMode(true);
            ctl.setWidgetCatalog({{QStringLiteral("RX"), QStringLiteral("RX Controls"),
                                   QStringLiteral("Receive")},
                                  {QStringLiteral("TX"), QStringLiteral("TX Controls"),
                                   QStringLiteral("Transmit")}});

            // Already on canvas -> refused.
            rx->setContainerVisible(true);
            if (!rx->isOnCanvas()) ctl.sendAppletToCanvas("RX");
            report("palette refuses an applet already on the canvas",
                   !ctl.addAppletFromPalette(QStringLiteral("RX"),
                                             QPointF(0.5, 0.5)));

            // Closed applet -> opened and placed at the click point.
            tx->setContainerVisible(false);
            report("palette opens a closed applet onto the canvas",
                   ctl.addAppletFromPalette(QStringLiteral("TX"),
                                            QPointF(0.25, 0.25))
                       && tx->isContainerVisible() && tx->isOnCanvas());
            const NormRect placed = canvas.itemRect("applet:TX");
            report("...centred on the click point",
                   qAbs((placed.x + placed.w / 2.0) - 0.25) < 0.05
                       && qAbs((placed.y + placed.h / 2.0) - 0.25) < 0.05);

            // Floating applet -> docked onto the canvas.
            mgr.floatContainer("TX");
            report("palette precondition: TX floats", tx->isFloating());
            report("palette docks a floating applet onto the canvas",
                   ctl.addAppletFromPalette(QStringLiteral("TX"),
                                            QPointF(0.7, 0.7))
                       && !tx->isFloating() && tx->isOnCanvas());

            // Locked canvas -> the palette still ADDS (review m3): with
            // the panel hidden it is the only mouse path to open an
            // applet, and opening a tool is operating.
            canvas.setEditMode(false);
            ctl.returnAppletToPanel("TX");
            report("palette adds while locked (m3)",
                   ctl.addAppletFromPalette(QStringLiteral("TX"),
                                            QPointF(0.5, 0.5))
                       && tx->isOnCanvas());
            canvas.setEditMode(true);

            // Hardware availability is a LIVE question, never a catalog
            // filter (the 8600 amplifier regression: TGXL/PGXL detection
            // is post-connect, but the catalog snapshot is taken at
            // construction — filtering there erased the Amplifiers
            // category, and the amps' recall membership, permanently).
            // The engine refuses while the hook says unavailable and
            // recovers the instant it flips — no re-snapshot needed.
            bool ampDetected = false;
            ctl.setWidgetAvailabilityHook(
                [&ampDetected](const QString& id) {
                    return id != QStringLiteral("TX") || ampDetected;
                });
            ctl.returnAppletToPanel("TX");
            tx->setContainerVisible(false);
            report("palette refuses undetected hardware (live hook)",
                   !ctl.addAppletFromPalette(QStringLiteral("TX"),
                                             QPointF(0.5, 0.5))
                       && !tx->isOnCanvas());
            ampDetected = true;
            report("palette recovers when detection flips, no re-wire",
                   ctl.addAppletFromPalette(QStringLiteral("TX"),
                                            QPointF(0.5, 0.5))
                       && tx->isOnCanvas());
            // paletteState() is the exact classification the Add-widget
            // menu renders (#4968 red-team M1: the menu is a stack-local
            // QMenu nothing can reach, so the logic lives where the suite
            // and the `workspace palette` verb can pin it).
            using PE = WorkspaceController::PaletteEntry;
            ctl.setWidgetCatalog(
                {{QStringLiteral("RX"), QStringLiteral("RX Controls"),
                  QStringLiteral("Receive")},
                 {QStringLiteral("TX"), QStringLiteral("TX Controls"),
                  QStringLiteral("Transmit")},
                 {QStringLiteral("GHOST"), QStringLiteral("Not Built"),
                  QStringLiteral("Receive")}});
            ctl.setWidgetAvailabilityHook(
                [](const QString& id) { return id != QStringLiteral("TX"); });
            auto stateOf = [&ctl](const QString& id) {
                for (const PE& e : ctl.paletteState())
                    if (e.id == id) return e.state;
                return PE::State::Absent;
            };
            // TX is undetected AND on the canvas: on-canvas must win
            // (#4968 M2 ordering — recall outranks availability, so a
            // placed applet shows "placed", never "unavailable").
            report("palette: on-canvas outranks not-detected",
                   tx->isOnCanvas()
                       && stateOf(QStringLiteral("TX")) == PE::State::OnCanvas);
            ctl.returnAppletToPanel("TX");
            tx->setContainerVisible(false);
            report("palette: undetected hardware classifies not-detected",
                   stateOf(QStringLiteral("TX")) == PE::State::NotDetected);
            report("palette: unconstructed applet classifies absent",
                   stateOf(QStringLiteral("GHOST")) == PE::State::Absent);
            if (rx->isOnCanvas()) ctl.returnAppletToPanel("RX");
            report("palette: available applet classifies addable",
                   stateOf(QStringLiteral("RX")) == PE::State::Addable);
            ctl.setWidgetAvailabilityHook({});
        }

        // ── Additional canvas windows (RFC #4887 phase 7) ────────────
        {
            ctl.switchWorkspace(wsA);
            ctl.setWidgetCatalog(
                {{QStringLiteral("RX"), QStringLiteral("RX Controls"),
                  QStringLiteral("Receive")},
                 {QStringLiteral("TX"), QStringLiteral("TX Controls"),
                  QStringLiteral("Transmit")}});
            rx->setContainerVisible(true);
            tx->setContainerVisible(true);
            if (!rx->isOnCanvas()) ctl.sendAppletToCanvas("RX");
            if (!tx->isOnCanvas()) ctl.sendAppletToCanvas("TX");
            // The LIVE widget for this pan id — an earlier block replaced
            // the original (slot reuse test), so never assert against the
            // stale panA pointer.
            QWidget* livePan = pans.value(QStringLiteral("0x40000000"));

            // The window host provides bare canvases — the controller
            // must never construct a real window; headless testability is
            // the seam's whole point (the PanHostHooks rule again).
            QMap<QString, WorkspaceCanvas*> wins;
            QStringList closedWins, destroyedWins;
            WorkspaceController::WindowHostHooks wh;
            wh.openWindow = [&](const QString& sid, const QString&,
                                const QByteArray&) -> WorkspaceCanvas* {
                if (!wins.contains(sid)) {
                    auto* c = new WorkspaceCanvas;
                    c->resize(1000, 800);
                    c->show();
                    wins.insert(sid, c);
                }
                return wins.value(sid);
            };
            wh.closeWindow = [&](const QString& sid) {
                closedWins.append(sid);   // hide — the canvas object stays
            };
            wh.destroyWindow = [&](const QString& sid) {
                delete wins.take(sid);
                destroyedWins.append(sid);
            };
            wh.geometryHint = [](const QString&) {
                return QByteArrayLiteral("HINT");
            };
            ctl.setWindowHost(wh);

            auto itemClosedInDoc = [&](const QString& itemId) {
                const QJsonDocument d =
                    QJsonDocument::fromJson(storedDocument().toUtf8());
                for (const QJsonValue& w :
                     d.object().value(QStringLiteral("workspaces")).toArray()) {
                    for (const QJsonValue& sv :
                         w.toObject().value(QStringLiteral("surfaces")).toArray()) {
                        for (const QJsonValue& iv :
                             sv.toObject().value(QStringLiteral("items")).toArray()) {
                            if (iv.toObject().value(QStringLiteral("id"))
                                    .toString() == itemId) {
                                return iv.toObject()
                                    .value(QStringLiteral("closed"))
                                    .toBool(false);
                            }
                        }
                    }
                }
                return false;
            };

            const QString sid =
                ctl.addCanvasWindow(QStringLiteral("Right monitor"));
            report("addCanvasWindow opens a window surface",
                   !sid.isEmpty() && wins.contains(sid));
            {
                bool listed = false, open = false;
                for (const auto& info : ctl.canvasWindowList())
                    if (info.id == sid) { listed = true; open = info.open; }
                report("...listed and open", listed && open);
            }
            report("...and persisted", storedDocument().contains(sid));

            // Cross-surface move: document first, widget follows.
            report("moveItemToSurface moves an applet's widget",
                   ctl.moveItemToSurface(QStringLiteral("applet:RX"), sid)
                       && rx->parentWidget() == wins.value(sid)
                       && wins.value(sid)->contains("applet:RX")
                       && !canvas.contains("applet:RX"));
            report("...surfaceHosting agrees",
                   ctl.surfaceHosting(QStringLiteral("applet:RX")) == sid);
            const QString panItem =
                ctl.panItemIdFor(QStringLiteral("0x40000000"));
            report("moveItemToSurface moves a pan's widget",
                   ctl.moveItemToSurface(panItem, sid)
                       && livePan->parentWidget() == wins.value(sid));
            report("...pans stay below applets on the new surface",
                   wins.value(sid)->layout().zOf(panItem) == 0);

            const NormRect extraPanBefore{0.11, 0.22, 0.33, 0.44};
            wins.value(sid)->setItemRect(panItem, extraPanBefore);
            ctl.commitPlacement();
            report("extra-surface pan is outside active-main layout membership",
                   !ctl.activeMainPanIdsForLayout().contains(
                       QStringLiteral("0x40000000")));
            report("pan layout leaves extra-surface geometry untouched",
                   ctl.applyPanLayout(QStringLiteral("4v"))
                       && wins.value(sid)->itemRect(panItem) == extraPanBefore);

            // A rect edit on the window canvas persists like any other.
            wins.value(sid)->setItemRect("applet:RX",
                                         NormRect{0.6, 0.6, 0.3, 0.3});
            ctl.commitPlacement();

            // HIDE-AND-KEEP (maintainer ruling): closing evicts
            // transiently — the applet shuts with NO closed flag recorded,
            // the pan returns to the stack — and reopening restores both.
            report("closing the window hides and keeps",
                   ctl.setCanvasWindowOpen(sid, false)
                       && closedWins.contains(sid)
                       && !rx->isContainerVisible()
                       && livePan->parentWidget() != wins.value(sid));
            report("...items stay recorded, closed flag ABSENT",
                   storedDocument().contains(QStringLiteral("applet:RX"))
                       && !itemClosedInDoc(QStringLiteral("applet:RX")));
            report("reopening re-places applet and pan",
                   ctl.setCanvasWindowOpen(sid, true)
                       && rx->isContainerVisible()
                       && rx->parentWidget() == wins.value(sid)
                       && livePan->parentWidget() == wins.value(sid));
            report("...at the edited rect",
                   nearly(wins.value(sid)->itemRect("applet:RX").x, 0.6,
                          0.05));

            // A switch closes windows the target lacks and reopens them on
            // the way back — with the arrangement intact.
            const int closesBefore = closedWins.size();
            const QString plain = ctl.createWorkspace(
                WorkspaceController::NewWorkspaceSource::Blank,
                QStringLiteral("NoWindows"));
            report("switching away closes the window (hide, not destroy)",
                   closedWins.size() > closesBefore
                       && destroyedWins.isEmpty()
                       && ctl.canvasWindowList().isEmpty());
            ctl.switchWorkspace(wsA);
            report("switching back reopens and re-places",
                   rx->parentWidget() == wins.value(sid)
                       && livePan->parentWidget() == wins.value(sid));
            ctl.deleteWorkspace(plain);

            // Removing the window moves its items home to main.
            report("removeCanvasWindow destroys and re-homes",
                   ctl.removeCanvasWindow(sid)
                       && destroyedWins.contains(sid)
                       && rx->isOnCanvas()
                       && rx->parentWidget() == &canvas
                       && livePan->parentWidget() == &canvas
                       && ctl.canvasWindowList().isEmpty());

            // Reset to Classic collapses extra surfaces: the stock shell
            // is one window.
            const QString sid2 =
                ctl.addCanvasWindow(QStringLiteral("Doomed"));
            ctl.moveItemToSurface(QStringLiteral("applet:TX"), sid2);
            ctl.resetToClassic();
            report("resetToClassic collapses extra windows",
                   destroyedWins.contains(sid2)
                       && ctl.canvasWindowList().isEmpty()
                       && !storedDocument().contains(sid2)
                       && tx->parentWidget() == &canvas);

            // A window created AFTER a destroy must come up FULLY WIRED.
            // The wire-once set keyed the deleted canvas by raw address; a
            // fresh allocation reusing it was silently skipped by
            // wireCanvas() and came up dead — no drops, no context menu,
            // no gesture persistence (e85f9c81).  Two cycles: the
            // allocator hands the SECOND fresh canvas the freed address
            // (red-team #4971 M6, patch adopted verbatim).  Edit-mode
            // mirroring is the cheapest observable only a live connection
            // can produce.
            {
                bool wired = true;
                for (int cycle = 0; cycle < 2; ++cycle) {
                    const QString sidN =
                        ctl.addCanvasWindow(QStringLiteral("Recycled"));
                    WorkspaceCanvas* fresh = wins.value(sidN);
                    if (!fresh) { wired = false; break; }
                    fresh->setEditMode(!canvas.isEditMode());
                    if (fresh->isEditMode() != canvas.isEditMode()) {
                        wired = false;   // its signal never reached us
                    }
                    ctl.removeCanvasWindow(sidN);
                }
                report("...and a canvas allocated after one is wired", wired);
            }

            // The GPU recipe brackets EVERY cross-top-level pan move
            // (red-team #4971 M5: nothing pinned it — deleting the hooks
            // from all five call sites left the suite green).  prepare
            // must precede the landing, finish must follow, and the
            // counts must balance across move, hide, reopen and remove.
            // Ordering is PER PAN, not global (red-team #4971 N1): the
            // replay paths deliberately BATCH — prepare(A) prepare(B) …
            // restoreItems … finish(A) finish(B) — so a global
            // alternation rule failed a correct implementation the
            // moment a window held two pans, and the batched multi-pan
            // shape (the realistic one) went unpinned.  Two live pans
            // exercise it; the invariants are per-id bracketing and
            // global balance.
            {
                QHash<QString, int> pending;   // panId -> open prepares
                int prep = 0, fin = 0;
                bool ordered = true;
                hooks.prepareTopLevelMove = [&](const QString& id) {
                    if (pending.value(id) != 0) ordered = false;
                    pending[id] += 1;
                    ++prep;
                };
                hooks.finishTopLevelMove = [&](const QString& id) {
                    if (pending.value(id) <= 0) ordered = false;
                    pending[id] -= 1;
                    ++fin;
                };
                ctl.setPanHost(hooks);

                auto* panG2 = new QWidget(&panOwner);
                pans.insert(QStringLiteral("0x40000050"), panG2);
                ctl.onPanAdded(QStringLiteral("0x40000050"));

                const QString sidG =
                    ctl.addCanvasWindow(QStringLiteral("GpuProbe"));
                const QString panItemG =
                    ctl.panItemIdFor(QStringLiteral("0x40000000"));
                const QString panItemH =
                    ctl.panItemIdFor(QStringLiteral("0x40000050"));
                ctl.moveItemToSurface(panItemG, sidG);
                ctl.moveItemToSurface(panItemH, sidG);
                const int afterMove = prep;
                ctl.setCanvasWindowOpen(sidG, false);   // batched evict
                ctl.setCanvasWindowOpen(sidG, true);    // batched re-place
                ctl.removeCanvasWindow(sidG);           // evict + re-home
                bool allClosed = true;
                for (int v : pending) {
                    if (v != 0) allClosed = false;
                }
                report("cross-top-level pan moves are GPU-bracketed, "
                       "batches included (M5/N1)",
                       afterMove >= 2 && prep >= 8 && prep == fin && ordered
                           && allClosed);

                ctl.onPanRemoved(QStringLiteral("0x40000050"));
                pans.remove(QStringLiteral("0x40000050"));
                hooks.prepareTopLevelMove = {};
                hooks.finishTopLevelMove  = {};
                ctl.setPanHost(hooks);   // detach before the locals die
            }

            // B1: an explicit palette surface RELOCATES a stale home.  TX
            // recorded closed on MAIN; adding it from a window's palette
            // must land it on THAT window, not resurrect it on main.
            {
                if (!tx->isContainerVisible()) tx->setContainerVisible(true);
                if (!tx->isOnCanvas()) ctl.sendAppletToCanvas("TX");
                tx->setContainerVisible(false);   // closed; home kept on main
                const QString sidB =
                    ctl.addCanvasWindow(QStringLiteral("B1Probe"));
                report("palette add onto a window relocates the home (B1)",
                       ctl.addAppletFromPalette(QStringLiteral("TX"),
                                                QPointF(0.5, 0.5), sidB)
                           && ctl.surfaceHosting(QStringLiteral("applet:TX"))
                                  == sidB
                           && tx->parentWidget() == wins.value(sidB)
                           && !canvas.contains("applet:TX"));

                // B2: a REFUSED add is a genuine no-op — hide the window,
                // then try an implicit add: the home is hidden, so it must
                // refuse BEFORE any write, leaving the document identical.
                ctl.setCanvasWindowOpen(sidB, false);
                const QString before = storedDocument();
                report("a refused add writes NOTHING (B2)",
                       !ctl.addAppletFromPalette(QStringLiteral("TX"),
                                                 QPointF(0.5, 0.5))
                           && storedDocument() == before);

                // M2: disable() must hand hide-and-keep's transient closes
                // back to the Classic shell — TX is recorded open on the
                // hidden window and currently shut.
                ctl.disable();
                report("disable reopens a hidden window's applets (M2)",
                       tx->isContainerVisible());
                ctl.enable({"RX", "TX"});
                canvas.setEditMode(true);

                // M4: moving an item OFF a hidden window is an operator
                // asking to SEE it — reopen and place, not a document-only
                // edit that resurrects later.
                report("move off a hidden window reopens and places (M4)",
                       ctl.moveItemToSurface(QStringLiteral("applet:TX"),
                                             WorkspaceSurface::kMainId)
                           && tx->isContainerVisible()
                           && tx->parentWidget() == &canvas
                           && ctl.surfaceHosting(QStringLiteral("applet:TX"))
                                  == WorkspaceSurface::kMainId);

                // N4: sibling verbs agree.  An unknown target surface is
                // an ERROR (not a silent landing on main), and an
                // EXPLICIT hidden target un-hides — the moveItemToSurface
                // rule, applied to the palette engine.
                report("add to an unknown surface refuses (N4)",
                       !ctl.addAppletFromPalette(QStringLiteral("TX"),
                                                 QPointF(0.5, 0.5),
                                                 QStringLiteral("nosuch"))
                           && ctl.surfaceHosting(QStringLiteral("applet:TX"))
                                  == WorkspaceSurface::kMainId);
                ctl.setCanvasWindowOpen(sidB, false);
                ctl.returnAppletToPanel("TX");
                tx->setContainerVisible(true);
                report("add to an explicitly-named hidden window un-hides "
                       "it (N4)",
                       ctl.addAppletFromPalette(QStringLiteral("TX"),
                                                QPointF(0.5, 0.5), sidB)
                           && tx->parentWidget() == wins.value(sidB)
                           && [&] {
                                  for (const auto& info :
                                       ctl.canvasWindowList()) {
                                      if (info.id == sidB) return info.open;
                                  }
                                  return false;
                              }());
                ctl.removeCanvasWindow(sidB);
            }

            ctl.setWindowHost({});
        }

        // Mode-off switch only retargets.
        ctl.switchWorkspace(wsA);
        rx->setContainerVisible(true);
        tx->setContainerVisible(true);
        ctl.disable();
        report("a mode-off switch retargets without touching widgets",
               ctl.switchWorkspace(wsC)
                   && ctl.activeWorkspaceId() == wsC
                   && rx->isContainerVisible());
        ctl.switchWorkspace(wsA);
    }

    // ── The field report: boot replay against a pre-layout canvas ────────
    //
    // The bug as reported: enable ran in the MainWindow constructor before
    // the first layout pass, the canvas had no real geometry, every item was
    // clamped to full-surface to satisfy pixel minimums, and the whole
    // session displayed one applet covering everything.  The stored document
    // survived only because the replay guard refused to write the wreckage
    // back.  Pinned here: a replay against a degenerate canvas must leave
    // stored rects untouched AND the display must recover on the first real
    // resize.
    {
        AppSettings::instance().removeStationValue(WorkspaceStore::kSettingsKey);
        AppSettings::instance().save();
        {
            // Author a document with a small applet rect, as the operator's
            // session did.
            WorkspaceStore author;
            bool migrated = false;
            author.loadOrMigrate({"RX", "TX"}, {}, &migrated);
            WorkspaceDocument doc = author.document();
            for (auto& ws : doc.workspaces) {
                for (auto& surf : ws.surfaces) {
                    for (auto& it : surf.items) {
                        if (it.id == QStringLiteral("applet:RX")) {
                            it.rect = NormRect{0.8, 0.1, 0.15, 0.2};
                        }
                    }
                }
            }
            doc.canvasEnabled = true;
            author.setDocument(doc);
            author.flush();
        }

        QWidget panel3;
        auto* lay3 = new QVBoxLayout(&panel3);
        ContainerManager mgr3(&panel3);
        auto* rx3 = mgr3.createContainer("RX", "RX");
        rx3->setContent(new QLabel("rx"));
        lay3->addWidget(rx3);
        rx3->setContainerVisible(true);
        panel3.show();

        // The canvas exists but has never been laid out — 0x0, exactly the
        // constructor-time state.
        WorkspaceCanvas coldCanvas;
        WorkspaceController coldCtl(&mgr3, &coldCanvas);
        // Deliberately NO pan host: the controller must behave with every
        // hook unset (a MainWindow that never wired pans).

        report("boot sees the enabled flag", coldCtl.boot());
        report("enable succeeds against an unsized canvas",
               coldCtl.enable({"RX", "TX"}));

        report("the stored rect survives the cold replay",
               coldCanvas.itemRect("applet:RX") == NormRect{0.8, 0.1, 0.15, 0.2});
        report("...and on disk too",
               storedDocument().contains(QStringLiteral("0.8")));

        // First real layout: the display lands where the document says.
        coldCanvas.resize(1000, 800);
        coldCanvas.show();
        QCoreApplication::processEvents();
        const QRect px = coldCanvas.itemWidget("applet:RX")->geometry();
        // 0.15 x 1000 = 150 px sits under the 160 px display floor, so the
        // VIEW widens to the floor — while the stored rect stays 0.15.
        // That split is the fix: display compromises, the document never does.
        report("the first real resize shows the arrangement, not wreckage",
               px == QRect(800, 80, 160, 160));
        report("...while the stored rect keeps its exact size",
               coldCanvas.itemRect("applet:RX") == NormRect{0.8, 0.1, 0.15, 0.2});
        report("no pan items appear without a pan host",
               coldCanvas.itemCount() == 1);

        coldCtl.disable();
    }

    // ── An unusable store fails the enable cleanly ───────────────────────
    {
        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                QStringLiteral("{ corrupt"));
        AppSettings::instance().save();

        WorkspaceCanvas canvas2;
        canvas2.resize(800, 600);
        canvas2.show();
        QWidget panel2;
        auto* lay2 = new QVBoxLayout(&panel2);
        ContainerManager mgr2(&panel2);
        auto* solo = mgr2.createContainer("SOLO", "Solo");
        solo->setContent(new QLabel("s"));
        lay2->addWidget(solo);
        solo->setContainerVisible(true);
        panel2.show();

        WorkspaceController ctl2(&mgr2, &canvas2);
        QString whyNot;
        report("enable against a corrupt store fails",
               !ctl2.enable({"SOLO"}, &whyNot));
        report("...and says why", !whyNot.isEmpty());
        report("...moving nothing",
               solo->isPanelDocked() && canvas2.itemCount() == 0);
        report("...and the corrupt document is untouched (write block)",
               storedDocument() == QStringLiteral("{ corrupt"));
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
