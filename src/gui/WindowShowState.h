#pragma once

class QWidget;

namespace AetherSDR {

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

// Bring w to the front, un-minimizing it first if needed.  showNormal() is
// guarded on isMinimized() because calling it unconditionally would clear a
// Maximized or FullScreen window (#3918).
void showAndRaiseWindow(QWidget* w);

} // namespace AetherSDR
