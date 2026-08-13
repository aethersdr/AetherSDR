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
using AetherSDR::ContainerManager;
using AetherSDR::ContainerWidget;
using AetherSDR::NormRect;
using AetherSDR::WorkspaceCanvas;
using AetherSDR::WorkspaceController;
using AetherSDR::WorkspaceDocument;
using AetherSDR::WorkspaceStore;

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

    // ── Disable: everything returns, placement is kept ───────────────────
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

        ctl.onPanRemoved(QStringLiteral("0x40000002"));
        pans.remove(QStringLiteral("0x40000002"));
        ctl.disable();
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
