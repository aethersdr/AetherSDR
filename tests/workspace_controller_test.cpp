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
#include "gui/workspace/WorkspaceStore.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <iostream>

using AetherSDR::AppSettings;
using AetherSDR::ContainerManager;
using AetherSDR::ContainerWidget;
using AetherSDR::NormRect;
using AetherSDR::WorkspaceCanvas;
using AetherSDR::WorkspaceController;
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

    QWidget panStack;
    WorkspaceController ctl(&mgr, &canvas);
    ctl.setPanStackWidget(&panStack);

    // ── Boot before any document exists ──────────────────────────────────
    report("boot with nothing stored asks for nothing", !ctl.boot());
    report("...and wrote no document", storedDocument().isEmpty());

    // ── First enable: migrate + place ────────────────────────────────────
    {
        QString whyNot;
        report("enable succeeds", ctl.enable({"RX", "TX"}, &whyNot));
        report("...and reports enabled", ctl.isEnabled());

        report("both applets are on the canvas",
               rx->isOnCanvas() && tx->isOnCanvas()
                   && canvas.contains("applet:RX") && canvas.contains("applet:TX"));
        report("...and left the panel layout",
               panelLayout->indexOf(rx) < 0 && panelLayout->indexOf(tx) < 0);
        report("the pan stack rides as the reserved item",
               canvas.contains(WorkspaceController::kPanStackItemId)
                   && panStack.parentWidget() == &canvas);
        report("the reserved pan area sits behind the applets",
               canvas.layout().zOf(WorkspaceController::kPanStackItemId) == 0);

        const QString doc = storedDocument();
        report("the document was flushed with the mode on",
               doc.contains(QStringLiteral("canvasEnabled"))
                   && doc.contains(QStringLiteral("panstack")));
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
        report("the pan stack is released parentless",
               panStack.parentWidget() == nullptr);
        report("the canvas is empty", canvas.itemCount() == 0);

        const QString doc = storedDocument();
        report("the mode is off on disk",
               !doc.contains(QStringLiteral("canvasEnabled")));
        report("...but placement is kept for re-enable",
               doc.contains(QStringLiteral("applet:TX")));

        report("re-enable places from the kept document",
               ctl.enable({"RX", "TX"}) && tx->isOnCanvas());
        report("...and boot on a fresh controller now asks for the mode",
               [&] {
                   WorkspaceStore probe;
                   return probe.load() && probe.document().canvasEnabled;
               }());
        ctl.disable();
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
