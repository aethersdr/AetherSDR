#pragma once

#include <QList>
#include <QString>
#include <Qt>

class QWidget;

namespace AetherSDR {

struct WindowMenuEntry {
    QWidget* window{nullptr};
    QString title;
    QString menuText;
};

// Window show-state helpers for press-to-open / press-again-to-close buttons.
//
// QWidget::isVisible() stays TRUE for a minimized window, and QWidget::show()
// on a minimized window restores it to its saved state — which is minimized.
// A toggle written as `if (w->isVisible()) w->hide(); else w->show();` is
// therefore unrecoverable once the window is minimized: the first press hides
// it, and the second press "shows" it straight back into the taskbar/Dock.
// Both behaviours are pinned by window_show_state_test.

// True when w is actually on screen for the user — visible AND not minimized.
[[nodiscard]] bool windowIsShowing(const QWidget* w);

// Bring w to the front, un-minimizing it first if needed.  Only the Minimized
// bit is cleared, so a Maximized or FullScreen window keeps that state — both
// when it is merely raised and when it is restored from the taskbar/Dock
// (#3918).  showNormal() would drop it in either case.
void showAndRaiseWindow(QWidget* w);

// True for window types that represent an operator-facing application
// window. Kept public so the offscreen test can pin types that its platform
// plugin normalizes when a synthetic QWidget is constructed.
[[nodiscard]] bool windowTypeAppearsInMenu(Qt::WindowType type);

// Return every visible, user-facing QWidget window owned by this process.
// The list is rebuilt from QApplication::topLevelWidgets() so modeless tools
// automatically join the Window menu without registering with MainWindow.
// Transient implementation windows are excluded. primaryWindow, when present,
// sorts first; the remainder sort by title. menuText includes duplicate
// numbering, minimized state, and escaped menu mnemonics.
[[nodiscard]] QList<WindowMenuEntry> windowInventory(
    const QWidget* primaryWindow = nullptr);

// Explicit candidates for deterministic inventory/filter/order coverage.
[[nodiscard]] QList<WindowMenuEntry> windowInventory(
    const QList<QWidget*>& candidates, const QWidget* primaryWindow);

// Window/canvas names are operator text, not QAction mnemonic markup.
[[nodiscard]] QString windowMenuText(QString title);

// Best available user-facing label for a top-level window.  Production
// windows normally provide windowTitle(); the fallbacks keep an unusual
// QWidget window reachable instead of silently omitting its native handle.
[[nodiscard]] QString windowMenuLabel(const QWidget* w);

} // namespace AetherSDR
