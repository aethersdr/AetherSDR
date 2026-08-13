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
#include "TitleBar.h"
#include "containers/ContainerManager.h"
#include "core/AppSettings.h"
#include "workspace/WorkspaceCanvas.h"
#include "workspace/WorkspaceController.h"

#include <QAction>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
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
    hooks.floatingPanGlobalRect = [this](const QString& id) -> QRect {
        if (auto* a = m_panStack->panadapter(id)) {
            if (QWidget* win = a->window(); win && win != this) {
                return win->geometry();
            }
        }
        return QRect();
    };
    hooks.requestDock = [this](const QString& id) {
        m_panStack->dockPanadapter(id);
    };
    hooks.recallGuard = [this](bool on) {
        m_appletPanel->setRecallInProgress(on);
    };
    m_workspaceController->setPanHost(hooks);

    // The widget palette (Add widget ▸): AppletPanel owns the applet
    // universe and the category taxonomy; the controller only renders it.
    {
        QList<WorkspaceController::WidgetCatalogEntry> catalog;
        for (const auto& e : m_appletPanel->appletCatalog()) {
            catalog.append({e.id, e.title, e.category});
        }
        m_workspaceController->setWidgetCatalog(catalog);
    }

    // Profile-bound recall (phase 6, decisions 6/8): a bound GLOBAL
    // profile switches the workspace; the status bar says so, because the
    // whole surface just changed and the operator deserves the why.
    connect(&m_radioModel, &RadioModel::profileLoadCompleted,
            m_workspaceController, &WorkspaceController::onRadioProfileLoaded);
    // Switches hide/recall the band stack; keep its indicator honest
    // (review M1).
    connect(m_workspaceController, &WorkspaceController::workspacesChanged,
            this, [this] { updateBandStackIndicator(); });
    connect(m_workspaceController,
            &WorkspaceController::workspaceSwitchedByProfile, this,
            [this](const QString& profile, const QString& wsLabel) {
                statusBar()->showMessage(
                    tr("Workspace \"%1\" (profile %2)").arg(wsLabel, profile),
                    6000);
            });

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
                // The panel's title-bar controls hide with the panel's role:
                // canvas mode owns arrangement (8600 field request).
                if (m_titleBar) {
                    m_titleBar->setAppletPanelControlsVisible(!on);
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
            if (m_appletPanel) {
                m_appletPanel->setVisible(
                    AppSettings::instance()
                        .value(QStringLiteral("AppletPanelVisible"),
                               QStringLiteral("True"))
                        .toString() == QLatin1String("True"));
            }
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

        // The panel hides with its controls (8600 field request): the
        // widget palette owns adding in canvas mode, and full recall owns
        // open/closed — a visible panel column is dead space.  VISUAL only:
        // the persisted AppletPanelVisible preference is untouched, so
        // classic mode returns exactly as the operator had it.  A floating
        // panel docks first via the canonical path (#2584's black-box
        // lesson).
        const bool panelWasFloating = (m_appletPanelFloatWindow != nullptr);
        if (panelWasFloating) {
            toggleAppletPanelFloating(false);
            // The dock above persisted AppletPanelFloating=False; the
            // operator didn't ask for that (review M2 — "visual only"
            // must cover the pop-out too).  Restore the preference AND
            // persist it — the toggle's own save already ran, so a bare
            // setValue sat cache-only and a crash lost the pop-out (found
            // by the 8600 bridge pass: the crash below made it real).
            AppSettings::instance().setValue(
                QStringLiteral("AppletPanelFloating"), QStringLiteral("True"));
            AppSettings::instance().save();
        }
        if (m_appletPanel) {
            m_appletPanel->hide();
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
    // Restore the panel per the operator's persisted preferences — not
    // unconditionally: someone who kept it hidden before the canvas keeps
    // it hidden after, and someone who had it POPPED OUT gets the float
    // back (review M2).
    if (m_appletPanel) {
        m_appletPanel->setVisible(
            AppSettings::instance()
                .value(QStringLiteral("AppletPanelVisible"),
                       QStringLiteral("True"))
                .toString() == QLatin1String("True"));
        if (AppSettings::instance()
                .value(QStringLiteral("AppletPanelFloating"),
                       QStringLiteral("False"))
                .toString() == QLatin1String("True")
            && !m_appletPanelFloatWindow) {
            // DEFERRED a turn: re-floating synchronously inside the same
            // event-loop turn as the shell swap created the float window
            // against surfaces mid-teardown — on Wayland that is a
            // wl_subsurface "no parent" protocol error and the compositor
            // KILLS the client (found live on the 8600 bridge pass).  One
            // turn lets Qt commit the reparented tree first — the same
            // deferral discipline the boot mount uses.
            QTimer::singleShot(0, this, [this] {
                if (m_workspaceController
                    && !m_workspaceController->isEnabled()
                    && !m_appletPanelFloatWindow) {
                    toggleAppletPanelFloating(true);
                }
            });
        }
    }
    if (m_panStack->count() > 1) {
        m_panStack->rearrangeLayout(
            AppSettings::instance()
                .value(QStringLiteral("PanadapterLayout"), QStringLiteral("1"))
                .toString());
    }
}

void MainWindow::rebuildWorkspaceSwitcherMenu(QMenu* menu)
{
    menu->clear();
    // clear() deletes actions but ORPHANS submenus (each owns its own
    // menuAction) — the bind submenu accumulated one QMenu per open
    // (review m5).
    qDeleteAll(menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
    if (!m_workspaceController) {
        return;
    }
    const QList<QPair<QString, QString>> list =
        m_workspaceController->workspaceList();
    const QString active = m_workspaceController->activeWorkspaceId();

    if (list.isEmpty()) {
        QAction* none = menu->addAction(tr("Enable the canvas to create workspaces"));
        none->setEnabled(false);
        return;
    }

    // Menu text is mnemonic text: double the ampersands in anything
    // data-driven (workspace labels, profile names) or Qt renders "&" as an
    // accelerator underline.
    auto menuText = [](const QString& t) {
        return QString(t).replace(QLatin1Char('&'), QStringLiteral("&&"));
    };
    for (const auto& [id, label] : list) {
        QAction* a = menu->addAction(menuText(label));
        a->setCheckable(true);
        a->setChecked(id == active);
        connect(a, &QAction::triggered, this, [this, id] {
            m_workspaceController->switchWorkspace(id);
        });
    }

    menu->addSeparator();
    auto askLabel = [this](const QString& title) {
        return QInputDialog::getText(this, title, tr("Name:"));
    };
    menu->addAction(tr("New from current layout…"), this, [this, askLabel] {
        const QString l = askLabel(tr("New workspace"));
        if (!l.isEmpty())
            m_workspaceController->createWorkspace(
                WorkspaceController::NewWorkspaceSource::Current, l);
    });
    menu->addAction(tr("New from Classic…"), this, [this, askLabel] {
        const QString l = askLabel(tr("New workspace"));
        if (!l.isEmpty())
            m_workspaceController->createWorkspace(
                WorkspaceController::NewWorkspaceSource::Classic, l);
    });
    menu->addAction(tr("New blank…"), this, [this, askLabel] {
        const QString l = askLabel(tr("New workspace"));
        if (!l.isEmpty())
            m_workspaceController->createWorkspace(
                WorkspaceController::NewWorkspaceSource::Blank, l);
    });

    menu->addSeparator();
    // "Active" is resolved AT TRIGGER TIME, not menu-build time (review,
    // K6OZY): a bound-profile recall arriving under an open menu used to
    // make "Delete active…" delete a different workspace than the dialog
    // named.
    menu->addAction(tr("Rename active…"), this, [this] {
        const QString now = m_workspaceController->activeWorkspaceId();
        QString nowLabel;
        for (const auto& [wid, wlabel] : m_workspaceController->workspaceList()) {
            if (wid == now) { nowLabel = wlabel; break; }
        }
        // Prefilled with the current name (review M4): an empty field made
        // retyping the same name the easy path, and that used to mangle it.
        const QString l = QInputDialog::getText(
            this, tr("Rename workspace"), tr("Name:"), QLineEdit::Normal,
            nowLabel);
        if (!l.isEmpty() && l != nowLabel)
            m_workspaceController->renameWorkspace(now, l);
    });
    QAction* del = menu->addAction(tr("Delete active…"), this, [this] {
        const QString now = m_workspaceController->activeWorkspaceId();
        QString nowLabel;
        for (const auto& [wid, wlabel] : m_workspaceController->workspaceList()) {
            if (wid == now) { nowLabel = wlabel; break; }
        }
        if (QMessageBox::question(
                this, tr("Delete workspace"),
                tr("Delete workspace \"%1\"? Its arrangement is lost.")
                    .arg(nowLabel))
            == QMessageBox::Yes) {
            m_workspaceController->deleteWorkspace(now);
        }
    });
    del->setEnabled(list.size() > 1);

    menu->addSeparator();
    menu->addAction(tr("Import pop-outs onto canvas"), this, [this] {
        const int n = m_workspaceController->importFloatingOntoCanvas();
        statusBar()->showMessage(
            n > 0 ? tr("%n pop-out(s) imported onto the canvas", nullptr, n)
                  : tr("No pop-outs to import"),
            5000);
    });

    // Bindings: which GLOBAL radio profiles recall the ACTIVE workspace.
    // Toggling binds/unbinds profile → active (decisions 5/6/8).
    menu->addSeparator();
    QMenu* bindMenu = menu->addMenu(tr("Bind to radio profile"));
    const QStringList profiles = m_radioModel.globalProfiles();
    if (profiles.isEmpty()) {
        QAction* none = bindMenu->addAction(tr("(no radio profiles)"));
        none->setEnabled(false);
    } else {
        for (const QString& prof : profiles) {
            QAction* b = bindMenu->addAction(menuText(prof));
            b->setCheckable(true);
            b->setChecked(m_workspaceController->boundWorkspaceFor(prof)
                          == active);
            connect(b, &QAction::toggled, this, [this, prof](bool on) {
                if (on) {
                    m_workspaceController->bindProfile(
                        prof, m_workspaceController->activeWorkspaceId());
                } else {
                    m_workspaceController->unbindProfile(prof);
                }
            });
        }
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
        // The active workspace, so two workspaces holding identical layouts
        // no longer give byte-identical status replies (review, K6OZY: a
        // harness diffing status to verify a switch passed when the switch
        // did nothing).
        out[QStringLiteral("activeWorkspace")] =
            m_workspaceController->activeWorkspaceId();
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

    if (action == QLatin1String("list")) {
        QVariantList wsList;
        for (const auto& [id, label] : m_workspaceController->workspaceList()) {
            QVariantMap w;
            w[QStringLiteral("id")]     = id;
            w[QStringLiteral("label")]  = label;
            w[QStringLiteral("active")] =
                (id == m_workspaceController->activeWorkspaceId());
            wsList.append(w);
        }
        out[QStringLiteral("workspaces")] = wsList;
        out[QStringLiteral("active")] = m_workspaceController->activeWorkspaceId();
        return out;
    }

    if (action == QLatin1String("switch")) {
        const QString target = args.trimmed();
        // An EXACT ID wins over a label match (review, K6OZY: labels are
        // operator text, and a workspace *labelled* "ws-1" used to hijack
        // `switch ws-1` away from the workspace whose id that is).
        QString id = target;
        bool exactId = false;
        for (const auto& [wsId, wsLabel] : m_workspaceController->workspaceList()) {
            if (wsId == target) { exactId = true; break; }
        }
        if (!exactId) {
            for (const auto& [wsId, wsLabel] :
                 m_workspaceController->workspaceList()) {
                if (wsLabel == target) { id = wsId; break; }
            }
        }
        if (!m_workspaceController->switchWorkspace(id)) {
            out[QStringLiteral("error")] =
                QStringLiteral("no such workspace: %1").arg(target);
            return out;
        }
        out[QStringLiteral("active")] = m_workspaceController->activeWorkspaceId();
        return out;
    }

    if (action == QLatin1String("create")) {
        const QStringList parts = args.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            out[QStringLiteral("error")] = QStringLiteral(
                "create wants <current|classic|blank> [label]");
            return out;
        }
        WorkspaceController::NewWorkspaceSource src;
        if (parts.first() == QLatin1String("current"))
            src = WorkspaceController::NewWorkspaceSource::Current;
        else if (parts.first() == QLatin1String("classic"))
            src = WorkspaceController::NewWorkspaceSource::Classic;
        else if (parts.first() == QLatin1String("blank"))
            src = WorkspaceController::NewWorkspaceSource::Blank;
        else {
            out[QStringLiteral("error")] = QStringLiteral(
                "create wants <current|classic|blank> [label]");
            return out;
        }
        const QString label = parts.mid(1).join(QLatin1Char(' '));
        const QString id = m_workspaceController->createWorkspace(src, label);
        if (id.isEmpty()) {
            out[QStringLiteral("error")] = QStringLiteral("create failed");
            return out;
        }
        out[QStringLiteral("id")] = id;
        // The ACTUAL label (deduplication may have suffixed it) and the
        // posture change — both script-observable state the reply used to
        // omit (review, K6OZY: a collision silently yielded "X (2)" and a
        // script binding by label touched the wrong workspace).
        for (const auto& [wsId, wsLabel] : m_workspaceController->workspaceList()) {
            if (wsId == id) { out[QStringLiteral("label")] = wsLabel; break; }
        }
        out[QStringLiteral("edit")]   = m_workspaceCanvas->isEditMode();
        out[QStringLiteral("active")] = m_workspaceController->activeWorkspaceId();
        return out;
    }

    if (action == QLatin1String("bind")) {
        // bind <workspaceId|-> <profile name with spaces>
        const QString a = args.trimmed();
        const int sp = a.indexOf(QLatin1Char(' '));
        if (sp <= 0) {
            out[QStringLiteral("error")] = QStringLiteral(
                "bind wants <workspaceId|-> <global profile name>");
            return out;
        }
        const QString wsId    = a.left(sp);
        const QString profile = a.mid(sp + 1).trimmed();
        if (wsId == QLatin1String("-")) {
            m_workspaceController->unbindProfile(profile);
        } else {
            m_workspaceController->bindProfile(profile, wsId);
            if (m_workspaceController->boundWorkspaceFor(profile) != wsId) {
                // The controller refused silently (unknown workspace); a
                // script must be able to tell "bound" from "ignored"
                // (review m4).
                out[QStringLiteral("error")] =
                    QStringLiteral("no such workspace: %1").arg(wsId);
                return out;
            }
        }
        out[QStringLiteral("bound")] =
            m_workspaceController->boundWorkspaceFor(profile);
        // Whether the radio currently REPORTS this profile.  Not an error —
        // binding before the radio connects is a legitimate workflow — but
        // a script can now catch the typo the Bind submenu cannot show
        // (review, K6OZY).
        out[QStringLiteral("knownProfile")] =
            m_radioModel.globalProfiles().contains(profile);
        return out;
    }

    if (action == QLatin1String("import-floats")) {
        if (!enabled) {
            out[QStringLiteral("error")] = QStringLiteral("canvas mode is off");
            return out;   // "imported: 0" must mean "nothing was floating"
        }
        out[QStringLiteral("imported")] =
            m_workspaceController->importFloatingOntoCanvas();
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
        QStringLiteral("unknown workspace action: %1 (status|enable|disable|"
                       "edit|place|list|switch|create|bind|import-floats)")
            .arg(action);
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
