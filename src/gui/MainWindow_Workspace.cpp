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
#include "PanadapterStack.h"
#include "containers/ContainerManager.h"
#include "workspace/WorkspaceCanvas.h"
#include "workspace/WorkspaceController.h"

#include <QAction>
#include <QSplitter>
#include <QStatusBar>

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
    if (m_workspaceController->boot()) {
        toggleWorkspaceCanvas(true);
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
        m_splitter->replaceWidget(panIdx, m_workspaceCanvas);
        m_workspaceCanvas->show();
        m_workspaceController->setPanStackWidget(m_panStack);

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

        // The stack is now a canvas item; the splitter slot follows
        // centralPanWidget() from here on.
        for (int i = 0; i < m_splitter->count(); ++i) {
            m_splitter->setStretchFactor(
                i, m_splitter->widget(i) == m_workspaceCanvas ? 1 : 0);
        }
        return;
    }

    // Disable: the controller returns every applet to its panel slot and
    // releases the pan stack parentless; the splitter takes both back.
    m_workspaceController->disable();

    const int canvasIdx = m_splitter->indexOf(m_workspaceCanvas);
    if (canvasIdx >= 0) {
        m_splitter->replaceWidget(canvasIdx, m_panStack);
    }
    m_workspaceCanvas->hide();
    m_panStack->show();
}

}  // namespace AetherSDR
