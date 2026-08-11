// DockMode::Canvas at the ContainerManager level (RFC #4887 phase 3a).
//
// The transitions are deliberately the same two halves float/dock use —
// detach-and-remember, restore-to-slot — so what is pinned here is the
// contract the WorkspaceController builds on: a container comes BACK to the
// slot it left (order preserved), a floating container docks before it goes
// to canvas, leaving the canvas is routed through the evictor, and the
// mode survives ContainerTree serialisation without breaking a downgrade.
//
// Offscreen, with a real AppSettings in a temporary home (saveState writes
// ContainerTree through it).

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/containers/ContainerManager.h"
#include "gui/containers/ContainerWidget.h"

#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <iostream>

using AetherSDR::AppSettings;
using AetherSDR::ContainerManager;
using AetherSDR::ContainerWidget;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-workspace-container-mode-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();

    // ── Detach and return, with the slot preserved ───────────────────────
    {
        QWidget panel;
        auto* lay = new QVBoxLayout(&panel);
        ContainerManager mgr(&panel);

        auto* a = mgr.createContainer("A", "A");
        auto* b = mgr.createContainer("B", "B");
        a->setContent(new QLabel("a"));
        b->setContent(new QLabel("b"));
        lay->addWidget(a);
        lay->addWidget(b);
        panel.show();

        report("a docked container detaches for the canvas",
               mgr.detachForCanvas("A") == a);
        report("...and reports canvas mode", a->isOnCanvas());
        report("...and left the panel layout", lay->indexOf(a) < 0);
        report("...leaving its sibling in place", lay->indexOf(b) == 0);

        report("a container already on canvas is refused",
               mgr.detachForCanvas("A") == nullptr);
        report("an unknown id is refused", mgr.detachForCanvas("nope") == nullptr);

        // The mode string reaches ContainerTree — the dual-write mirror.
        const QString tree =
            AppSettings::instance().value(QStringLiteral("ContainerTree")).toString();
        report("the canvas mode serialises", tree.contains(QStringLiteral("\"canvas\"")));

        mgr.returnFromCanvas("A", a);
        report("returning restores panel mode", a->isPanelDocked());
        report("...at the slot it left, not the end", lay->indexOf(a) == 0);
        report("...with its sibling after it", lay->indexOf(b) == 1);

        report("returning a container not on canvas is a no-op",
               (mgr.returnFromCanvas("A", a), a->isPanelDocked() && lay->indexOf(a) == 0));
    }

    // ── The width cap lifts on canvas and comes back on return (#3451) ───
    {
        QWidget panel;
        auto* lay = new QVBoxLayout(&panel);
        ContainerManager mgr(&panel);

        auto* c = mgr.createContainer("W", "W");
        auto* content = new QLabel("w");
        content->setMaximumWidth(260);   // the panel's fixed-column cap
        c->setContent(content);
        lay->addWidget(c);
        panel.show();

        mgr.detachForCanvas("W");
        report("the docked width cap lifts on canvas",
               content->maximumWidth() == QWIDGETSIZE_MAX);

        mgr.returnFromCanvas("W", c);
        report("...and is restored on return", content->maximumWidth() == 260);
    }

    // ── Floating containers dock before they reach the canvas ────────────
    {
        QWidget panel;
        auto* lay = new QVBoxLayout(&panel);
        ContainerManager mgr(&panel);

        auto* c = mgr.createContainer("F", "F");
        c->setContent(new QLabel("f"));
        lay->addWidget(c);
        panel.show();

        mgr.floatContainer("F");
        report("the container floats first", c->isFloating());

        ContainerWidget* detached = mgr.detachForCanvas("F");
        report("a floating container still detaches for canvas", detached == c);
        report("...arriving in canvas mode, not floating",
               c->isOnCanvas() && !c->isFloating());

        mgr.returnFromCanvas("F", c);
        report("...and still returns to its panel slot",
               c->isPanelDocked() && lay->indexOf(c) == 0);
    }

    // ── Leaving the canvas goes through the evictor ──────────────────────
    {
        QWidget panel;
        auto* lay = new QVBoxLayout(&panel);
        ContainerManager mgr(&panel);

        auto* c = mgr.createContainer("E", "E");
        c->setContent(new QLabel("e"));
        lay->addWidget(c);
        panel.show();

        int evictions = 0;
        mgr.setCanvasEvictor([&](const QString& id) {
            ++evictions;
            // What the controller does: off the canvas, back to the panel.
            if (ContainerWidget* cc = mgr.container(id)) {
                mgr.returnFromCanvas(id, cc);
            }
        });

        mgr.detachForCanvas("E");
        report("the container is on canvas", c->isOnCanvas());

        // Float-from-canvas: the evictor runs first, then the float proceeds
        // from the restored panel slot — so the float records a REAL slot to
        // dock back to.
        mgr.floatContainer("E");
        report("floating from canvas evicts first", evictions == 1);
        report("...and the float then proceeds", c->isFloating());

        mgr.dockContainer("E");
        report("...and docking lands back in the panel slot",
               c->isPanelDocked() && lay->indexOf(c) == 0);

        // The title-bar "return to panel" button emits dockRequested; for a
        // canvas container the manager routes that through the evictor too.
        mgr.detachForCanvas("E");
        QMetaObject::invokeMethod(c, "dockRequested");
        report("a canvas dock request routes through the evictor",
               evictions == 2 && c->isPanelDocked() && lay->indexOf(c) == 0);
    }

    // ── A stored "canvas" mode restores as panel-docked ──────────────────
    //
    // restoreState() runs before any canvas exists; the document re-places
    // items when the mode is enabled.  A stored canvas mode must therefore
    // land panel-docked, not crash and not float.
    {
        AppSettings::instance().setValue(
            QStringLiteral("ContainerTree"),
            QStringLiteral(R"({"version":1,"containers":{)"
                           R"("R":{"mode":"canvas","visible":true,)"
                           R"("contentType":"","parent":""}}})"));

        QWidget panel;
        new QVBoxLayout(&panel);
        ContainerManager mgr(&panel);
        mgr.restoreState();

        ContainerWidget* r = mgr.container("R");
        report("restore creates the container", r != nullptr);
        report("a stored canvas mode restores as panel-docked",
               r && r->isPanelDocked() && !r->isFloating());
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
