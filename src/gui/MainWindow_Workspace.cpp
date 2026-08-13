// MainWindow_Workspace.cpp — the workspace canvas mount (RFC #4887 phase 3).
//
// Everything MainWindow contributes to canvas mode lives here: creating the
// canvas + controller, the View-menu toggle, and the one structural move —
// swapping the canvas into the splitter slot the PanadapterStack normally
// occupies, with the stack riding along as a canvas item.  Placement policy
// (what a drop means, which applets belong on the canvas, what the document
// says) is WorkspaceController's business, deliberately not this file's.
//
// Sibling TU per docs/architecture/mainwindow-decomposition.md — methods are
// MainWindow:: members declared in MainWindow.h.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "BandStackPanel.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "containers/ContainerManager.h"
#include "core/AppSettings.h"
#include "workspace/WorkspaceCanvas.h"
#include "workspace/WorkspaceController.h"

#include <QAction>
#include <QVariantList>
#include <QVariantMap>
#include <QSplitter>
#include <QTimer>

namespace AetherSDR {

QWidget* MainWindow::centralPanWidget() const
{
    // The splitter's centre slot: the canvas when canvas mode holds the pan
    // stack, the pan stack itself otherwise.  Splitter size/stretch code
    // compares against this so both arrangements share one path.
    if (m_workspaceController && m_workspaceController->isEnabled()) {
        return m_workspaceCanvas;
    }
    return m_panStack;
}

void MainWindow::wireWorkspaceCanvas()
{
    // Owned by the window from birth (review note: parentless, it leaked on
    // never-enabled installs and after every disable) — and being a child
    // of the same top level is ALSO what makes every mount move below a
    // same-top-level reparent (red-team B1).
    m_workspaceCanvas = new WorkspaceCanvas(this);
    m_workspaceCanvas->hide();   // mounted on demand by toggleWorkspaceCanvas()

    m_workspaceController = new WorkspaceController(
        m_appletPanel->containerManager(), m_workspaceCanvas, this);

    connect(m_appletPanel, &AppletPanel::canvasReturnRequested,
            m_workspaceController, &WorkspaceController::returnAppletToPanel);
    // A live move released over the panel returns the applet to it.
    m_workspaceController->setReturnTarget(m_appletPanel);

    // ── Pans as items (RFC #4887 phase 4) ────────────────────────────────
    //
    // The controller's pan seam is callbacks + slots, never a stack pointer:
    // policy stays unit-testable against plain widgets.  These lambdas are
    // the only place the two types meet.
    WorkspaceController::PanHostHooks hooks;
    hooks.panIds     = [this] { return m_panStack->panIds(); };
    hooks.detach     = [this](const QString& id) -> QWidget* {
        return m_panStack->detachForCanvas(id);
    };
    hooks.restore    = [this](const QString& id, QWidget* w) {
        m_panStack->returnFromCanvas(id, qobject_cast<PanadapterApplet*>(w));
    };
    hooks.isFloating = [this](const QString& id) {
        return m_panStack->isFloating(id);
    };
    hooks.requestFloat = [this](const QString& id) {
        m_panStack->floatPanadapter(id);
    };
    hooks.bandStack = [this]() -> QWidget* {
        return m_panStack->bandStackPanel();
    };
    hooks.reclaimBandStack = [this](QWidget*) {
        m_panStack->reclaimBandStackPanel();
    };
    m_workspaceController->setPanHost(hooks);

    connect(m_panStack, &PanadapterStack::panAdded,
            m_workspaceController, &WorkspaceController::onPanAdded);
    connect(m_panStack, &PanadapterStack::panRemoved,
            m_workspaceController, &WorkspaceController::onPanRemoved);
    connect(m_panStack, &PanadapterStack::panRekeyed,
            m_workspaceController, &WorkspaceController::onPanRekeyed);
    connect(m_panStack, &PanadapterStack::panFloated,
            m_workspaceController, &WorkspaceController::onPanFloated);
    connect(m_panStack, &PanadapterStack::panDocked,
            m_workspaceController, &WorkspaceController::onPanDocked);

    // Each applet's title-strip live-move stream feeds the canvas gesture.
    // The applet pointer is captured (not the id) so a band-recall rekey
    // keeps the stream on the right item.
    auto wirePanApplet = [this](PanadapterApplet* a) {
        if (!a) {
            return;
        }
        connect(a, &PanadapterApplet::canvasDragBegan,
                m_workspaceController, [this, a](const QPoint& g) {
                    m_workspaceController->beginPanItemMove(a->panId(), g);
                });
        connect(a, &PanadapterApplet::canvasDragMoved,
                m_workspaceController, [this](const QPoint& g) {
                    m_workspaceController->movePanItem(g);
                });
        connect(a, &PanadapterApplet::canvasDragEnded,
                m_workspaceController, [this](const QPoint& g) {
                    m_workspaceController->endPanItemMove(g);
                });
    };
    // Pre-existing applets FIRST — the same rule the controller ctor
    // follows for containers.  buildUI() creates the "default" placeholder
    // pan long before this method runs, and the radio REKEYS that applet
    // into its first real pan, so panAdded never fires for it: without
    // this loop the PRIMARY pan of every session has a dead title strip
    // (the 8600 "can't drag the pan" report) while pans 2+ wire normally.
    // No double-wiring: addPanadapter() early-returns an existing applet
    // without re-emitting panAdded.
    for (PanadapterApplet* a : m_panStack->allApplets()) {
        wirePanApplet(a);
    }
    connect(m_panStack, &PanadapterStack::panAdded, this,
            [this, wirePanApplet](const QString& panId) {
                wirePanApplet(m_panStack->panadapter(panId));
            });

    connect(m_workspaceController, &WorkspaceController::enabledChanged,
            this, [this](bool on) {
                if (m_workspaceCanvasAction
                    && m_workspaceCanvasAction->isChecked() != on) {
                    QSignalBlocker blocker(m_workspaceCanvasAction);
                    m_workspaceCanvasAction->setChecked(on);
                }
                // Edit Layout is meaningful only while the canvas is up.
                if (m_workspaceEditAction) {
                    m_workspaceEditAction->setEnabled(on);
                }
            });

    // Edit Layout ↔ canvas edit mode, both directions: the context menu's
    // toggle and the controller's first-enable policy also flip the mode,
    // and the menu action must stay honest.
    if (m_workspaceEditAction) {
        connect(m_workspaceEditAction, &QAction::toggled, this, [this](bool on) {
            m_workspaceCanvas->setEditMode(on);
        });
        connect(m_workspaceCanvas, &WorkspaceCanvas::editModeChanged, this,
                [this](bool on) {
                    if (m_workspaceEditAction->isChecked() != on) {
                        QSignalBlocker blocker(m_workspaceEditAction);
                        m_workspaceEditAction->setChecked(on);
                    }
                });
    }

    // The stored document decides whether the mode comes back up.  boot()
    // never migrates — a fresh install that has never enabled the canvas
    // must not gain a Workspaces key just by launching.
    //
    // The mount itself is DEFERRED one event-loop turn: this runs in the
    // MainWindow constructor, before show() and the first layout pass, and
    // replaying the document onto a canvas with no real geometry is exactly
    // how the phase 3 field report broke — every item displayed full-canvas
    // for the whole session.  The model is bounds-only now so a degenerate
    // size can no longer corrupt anything, but mounting after layout means
    // the first frame the operator sees is the right one.
    if (m_workspaceController->boot()) {
        QTimer::singleShot(0, this, [this] { toggleWorkspaceCanvas(true); });
    }
}

void MainWindow::toggleWorkspaceCanvas(bool on)
{
    if (!m_workspaceController || !m_splitter || !m_panStack) {
        return;
    }
    if (on == m_workspaceController->isEnabled()) {
        return;
    }

    if (on) {
        // Mount first, then enable: the controller places items against the
        // canvas's real size, so the canvas has to be in the splitter (and
        // sized by it) before the document is replayed onto it.
        //
        // The swap is a same-top-level reparent, which is the SAFE pattern
        // for the QRhiWidget spectrum children (#4091): the backing-store
        // QRhi never changes, so no prepare/reset dance is needed — and
        // deliberately none is done, because forcing one here would be the
        // destroy/recreate storm that crashed the 2016-era Intel D3D11 UMD.
        const int panIdx = m_splitter->indexOf(m_panStack);
        if (panIdx < 0) {
            return;
        }
        // Session-transient, so read it BEFORE the stack goes hidden.
        const bool bandStackWasVisible =
            m_panStack->bandStackPanel()
            && m_panStack->bandStackPanel()->isVisibleTo(m_panStack);

        // EVERY move below is a ONE-STEP, SAME-TOP-LEVEL reparent — the
        // #2495-safe pattern for the QRhiWidget children riding inside the
        // stack.  No widget ever passes through setParent(nullptr): a
        // parentless QWidget IS a transient top-level, and taking the
        // stack's live QRhi children through one without float/dock's
        // prepare/reset dance is the #1344/#4091 hazard (red-team B1 —
        // the previous shape did exactly that, twice per toggle).  The
        // canvas is a child of this window from construction, so
        // stack→canvas and canvas→splitter both stay inside one top-level.
        //
        // Since phase 4 the stack is not an item — its applets are.  It
        // rides hidden as the pans' owner (creation, wiring, float/dock,
        // render scheduling); enable() borrows each applet onto the canvas.
        m_panStack->setParent(m_workspaceCanvas);
        m_panStack->hide();
        m_splitter->insertWidget(panIdx, m_workspaceCanvas);
        m_workspaceCanvas->show();

        QString whyNot;
        if (!m_workspaceController->enable(m_appletPanel->appletIds(), &whyNot)) {
            // Put the shell back exactly as it was and say why, without a
            // modal in the way of whatever the operator was doing.  The
            // write-blocked newer-document case lands here (PR #4900 H1).
            // Same one-step discipline in reverse — the previous rollback
            // called replaceWidget(canvas, <canvas's own child>), the
            // exact wedged-shell shape the reviews flagged, on the one
            // path that runs when something already went wrong.
            m_panStack->setParent(this);
            const int backIdx = m_splitter->indexOf(m_workspaceCanvas);
            if (backIdx >= 0) {
                m_splitter->replaceWidget(backIdx, m_panStack);
            } else {
                m_splitter->insertWidget(panIdx, m_panStack);
            }
            m_workspaceCanvas->setParent(this);
            m_workspaceCanvas->hide();
            m_panStack->show();
            if (m_workspaceCanvasAction) {
                QSignalBlocker blocker(m_workspaceCanvasAction);
                m_workspaceCanvasAction->setChecked(false);
            }
            statusBar()->showMessage(
                tr("Workspace canvas unavailable: %1").arg(whyNot), 8000);
            return;
        }

        if (bandStackWasVisible) {
            m_workspaceController->setBandStackVisible(true);
        }

        for (int i = 0; i < m_splitter->count(); ++i) {
            m_splitter->setStretchFactor(
                i, m_splitter->widget(i) == m_workspaceCanvas ? 1 : 0);
        }
        return;
    }

    // Disable: the controller returns every applet to its panel slot and
    // every pan applet into the stack's splitter (the band stack too); the
    // splitter takes the stack back, and the operator's saved pan layout is
    // re-applied so Classic comes back as the arrangement it was, not as a
    // flat column of whatever order the returns happened in.
    m_workspaceController->disable();

    // The stack is a CHILD of the canvas while the mode is on, so it steps
    // to this window first (one-step, same-top-level — never through
    // nullptr, red-team B1), then swaps into the canvas's slot.  The
    // canvas is re-owned by this window afterwards: replaceWidget() drops
    // its parent, and an unowned canvas leaks across mode cycles and
    // never-enabled sessions (review note).
    const int canvasIdx = m_splitter->indexOf(m_workspaceCanvas);
    m_panStack->setParent(this);
    if (canvasIdx >= 0) {
        m_splitter->replaceWidget(canvasIdx, m_panStack);
    } else {
        // The canvas is somehow already out of the splitter — never leave
        // the shell without a centre widget (review note).
        m_splitter->insertWidget(0, m_panStack);
    }
    m_workspaceCanvas->setParent(this);
    m_workspaceCanvas->hide();
    m_panStack->show();
    if (m_panStack->count() > 1) {
        m_panStack->rearrangeLayout(
            AppSettings::instance()
                .value(QStringLiteral("PanadapterLayout"), QStringLiteral("1"))
                .toString());
    }
}

QVariantMap MainWindow::automationWorkspace(const QString& action,
                                            const QString& args)
{
    QVariantMap out;
    if (!m_workspaceController || !m_workspaceCanvas) {
        out[QStringLiteral("error")] = QStringLiteral("workspace canvas not wired");
        return out;
    }
    const bool enabled = m_workspaceController->isEnabled();

    if (action == QLatin1String("status") || action == QLatin1String("query")) {
        out[QStringLiteral("enabled")]  = enabled;
        out[QStringLiteral("edit")]     = m_workspaceCanvas->isEditMode();
        out[QStringLiteral("gridSnap")] = m_workspaceCanvas->isGridSnapEnabled();
        out[QStringLiteral("selected")] = m_workspaceCanvas->selectedItem();
        QVariantList items;
        for (const CanvasItem& it : m_workspaceCanvas->layout().itemsByZ()) {
            QVariantMap m;
            m[QStringLiteral("id")]   = it.id;
            m[QStringLiteral("type")] = it.contentType;
            m[QStringLiteral("x")]    = it.rect.x;
            m[QStringLiteral("y")]    = it.rect.y;
            m[QStringLiteral("w")]    = it.rect.w;
            m[QStringLiteral("h")]    = it.rect.h;
            m[QStringLiteral("z")]    = m_workspaceCanvas->layout().zOf(it.id);
            // Whether the widget is REALLY a canvas child right now — the
            // model can be healthy while a stack rebuild has reclaimed the
            // widget (the 8600 theft), and this is how a smoke proves the
            // difference without trusting the model it is auditing.
            QWidget* w = m_workspaceCanvas->itemWidget(it.id);
            m[QStringLiteral("hosted")] =
                (w && w->parentWidget() == m_workspaceCanvas);
            items.append(m);
        }
        out[QStringLiteral("items")] = items;
        out[QStringLiteral("count")] = items.size();
        return out;
    }

    if (action == QLatin1String("enable") || action == QLatin1String("disable")) {
        const bool want = (action == QLatin1String("enable"));
        if (want != enabled) {
            toggleWorkspaceCanvas(want);
        }
        const bool now = m_workspaceController->isEnabled();
        out[QStringLiteral("enabled")] = now;
        if (want && !now) {
            // The refusal reason went to the status bar (the H1 path);
            // dumpTree's statusMessage carries it for asserting.
            out[QStringLiteral("error")] =
                QStringLiteral("enable refused — see statusMessage");
        }
        return out;
    }

    if (action == QLatin1String("edit")) {
        // Operator posture only — `place` stays available in BOTH postures
        // (the bridge is not an operator, and tests must be able to arrange
        // a locked canvas).
        const QString v = args.trimmed().toLower();
        if (v == QLatin1String("on") || v == QLatin1String("off")) {
            m_workspaceCanvas->setEditMode(v == QLatin1String("on"));
        } else if (!v.isEmpty()) {
            out[QStringLiteral("error")] =
                QStringLiteral("edit wants on|off (or nothing to query)");
            return out;
        }
        out[QStringLiteral("edit")] = m_workspaceCanvas->isEditMode();
        return out;
    }

    if (action == QLatin1String("place")) {
        if (!enabled) {
            out[QStringLiteral("error")] = QStringLiteral("canvas mode is off");
            return out;
        }
        const QStringList parts =
            args.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 3 && parts.size() != 5) {
            out[QStringLiteral("error")] = QStringLiteral(
                "place wants <itemId> <x> <y> [w h] (canvas fractions)");
            return out;
        }
        const QString itemId = parts.at(0);
        if (!m_workspaceCanvas->contains(itemId)) {
            out[QStringLiteral("error")] =
                QStringLiteral("no such item: %1").arg(itemId);
            return out;
        }
        bool okX = false, okY = false, okW = true, okH = true;
        NormRect r = m_workspaceCanvas->itemRect(itemId);
        r.x = parts.at(1).toDouble(&okX);
        r.y = parts.at(2).toDouble(&okY);
        if (parts.size() == 5) {
            r.w = parts.at(3).toDouble(&okW);
            r.h = parts.at(4).toDouble(&okH);
        }
        if (!okX || !okY || !okW || !okH) {
            out[QStringLiteral("error")] = QStringLiteral("unparseable coordinates");
            return out;
        }
        // Reject, don't clamp, hostile numerics (red-team M2): inf/nan and
        // zero/negative sizes used to collapse to a {0,0,0,0} rect that was
        // persisted — a permanently unhittable item reported as ok:true.
        // setItemRect refuses invalid rects too now; this check exists so
        // the CALLER hears why instead of a silent no-op.
        if (!r.isValid()) {
            out[QStringLiteral("error")] = QStringLiteral(
                "coordinates must be finite with positive size");
            return out;
        }
        m_workspaceCanvas->setItemRect(itemId, r);   // clamped; the
        m_workspaceController->commitPlacement();    // controller records it
        const NormRect applied = m_workspaceCanvas->itemRect(itemId);
        out[QStringLiteral("id")] = itemId;
        out[QStringLiteral("x")]  = applied.x;
        out[QStringLiteral("y")]  = applied.y;
        out[QStringLiteral("w")]  = applied.w;
        out[QStringLiteral("h")]  = applied.h;
        return out;
    }

    out[QStringLiteral("error")] =
        QStringLiteral("unknown workspace action: %1 "
                       "(status|enable|disable|edit|place)").arg(action);
    return out;
}

void MainWindow::setBandStackPanelVisible(bool show)
{
    if (m_workspaceController && m_workspaceController->isEnabled()) {
        m_workspaceController->setBandStackVisible(show);
        return;
    }
    if (m_panStack) {
        m_panStack->setBandStackVisible(show);
    }
}

}  // namespace AetherSDR
