#pragma once

#include "PersistentDialog.h"

class QVBoxLayout;

namespace AetherSDR {

class CwxPanel;

// Top-level, resizable host for a popped-out CwxPanel. Built on
// PersistentDialog for the standard geometry-persist + frameless-chrome
// boilerplate (see docs/style/dialog-patterns.md) — CwxFloatingWindow only
// adds the adopt/take reparenting dance, since (unlike a typical
// PersistentDialog subclass) its content is an existing, model-bound
// CwxPanel that must return intact to MainWindow's splitter on dock rather
// than being destroyed with the window.
//
// Created by MainWindow::floatCwxPanel(), destroyed by
// MainWindow::dockCwxPanel() — mirrors PanFloatingWindow's lifecycle for
// panadapters, minus the GPU-surface teardown that class needs and CWX's
// plain QWidget content does not.
class CwxFloatingWindow : public PersistentDialog {
    Q_OBJECT

public:
    explicit CwxFloatingWindow(QWidget* parent = nullptr);

    void adoptPanel(CwxPanel* panel);
    CwxPanel* takePanel();
    CwxPanel* panel() const { return m_panel; }

signals:
    // Emitted when the window is closed (title-bar X, Alt+F4, ...) instead
    // of actually closing — MainWindow docks the panel back into the
    // splitter in response, matching PanFloatingWindow's dockRequested.
    void dockRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    CwxPanel*    m_panel{nullptr};
    QVBoxLayout* m_layout{nullptr};
};

} // namespace AetherSDR
