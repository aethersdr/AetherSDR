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
#include <QSplitter>
#include <QStatusBar>
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
    m_workspaceCanvas = new WorkspaceCanvas;
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
    // Wired per applet at creation; the applet pointer is captured (not the
    // id) so a band-recall rekey keeps the stream on the right item.
    connect(m_panStack, &PanadapterStack::panAdded, this,
            [this](const QString& panId) {
                auto* a = m_panStack->panadapter(panId);
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
            });

    connect(m_workspaceController, &WorkspaceController::enabledChanged,
            this, [this](bool on) {
                if (m_workspaceCanvasAction
                    && m_workspaceCanvasAction->isChecked() != on) {
                    QSignalBlocker blocker(m_workspaceCanvasAction);
                    m_workspaceCanvasAction->setChecked(on);
                }
            });

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

        m_splitter->replaceWidget(panIdx, m_workspaceCanvas);
        m_workspaceCanvas->show();
        // Since phase 4 the stack is not an item — its applets are.  It
        // rides hidden as the pans' owner (creation, wiring, float/dock,
        // render scheduling); enable() borrows each applet onto the canvas.
        m_panStack->setParent(m_workspaceCanvas);
        m_panStack->hide();

        QString whyNot;
        if (!m_workspaceController->enable(m_appletPanel->appletIds(), &whyNot)) {
            // Put the shell back exactly as it was and say why, without a
            // modal in the way of whatever the operator was doing.  The
            // write-blocked newer-document case lands here (PR #4900 H1).
            m_splitter->replaceWidget(m_splitter->indexOf(m_workspaceCanvas),
                                      m_panStack);
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

    const int canvasIdx = m_splitter->indexOf(m_workspaceCanvas);
    if (canvasIdx >= 0) {
        m_splitter->replaceWidget(canvasIdx, m_panStack);
    }
    m_workspaceCanvas->hide();
    m_panStack->show();
    if (m_panStack->count() > 1) {
        m_panStack->rearrangeLayout(
            AppSettings::instance()
                .value(QStringLiteral("PanadapterLayout"), QStringLiteral("1"))
                .toString());
    }
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
