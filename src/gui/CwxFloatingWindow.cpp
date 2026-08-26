#include "CwxFloatingWindow.h"
#include "CwxPanel.h"

#include <QCloseEvent>
#include <QVBoxLayout>

namespace AetherSDR {

CwxFloatingWindow::CwxFloatingWindow(QWidget* parent)
    : PersistentDialog("CWX", "CwxFloatingGeometry", parent)
{
    setMinimumSize(220, 260);

    m_layout = new QVBoxLayout(bodyWidget());
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    setBodyLayoutMargins({0, 0, 0, 0}, {0, 0, 0, 0});
}

void CwxFloatingWindow::adoptPanel(CwxPanel* panel)
{
    if (!panel) return;
    m_panel = panel;

    // addWidget() calls setParent() internally, so the panel goes straight
    // from the splitter to this window without an intermediate top-level
    // state (same reasoning as PanFloatingWindow::adoptApplet).
    m_layout->addWidget(m_panel, 1);
}

CwxPanel* CwxFloatingWindow::takePanel()
{
    if (!m_panel) return nullptr;
    auto* p = m_panel;
    m_layout->removeWidget(p);
    p->setParent(nullptr);
    m_panel = nullptr;
    return p;
}

void CwxFloatingWindow::closeEvent(QCloseEvent* event)
{
    // Run the base class's geometry-save-and-accept first (crash-resilient
    // disk flush, matching every other PersistentDialog), then override its
    // acceptance: CWX doesn't destroy on close, it docks back into
    // MainWindow's splitter — mirrors PanFloatingWindow::closeEvent.
    PersistentDialog::closeEvent(event);
    emit dockRequested();
    event->ignore();
}

} // namespace AetherSDR
