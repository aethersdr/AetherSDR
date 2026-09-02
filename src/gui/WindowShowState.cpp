#include "gui/WindowShowState.h"

#include <QWidget>

namespace AetherSDR {

bool windowIsShowing(const QWidget* w)
{
    return w && w->isVisible() && !w->isMinimized();
}

void showAndRaiseWindow(QWidget* w)
{
    if (!w)
        return;
    // Clear ONLY the minimized bit.  showNormal() would also clear Maximized
    // and FullScreen, so a strip that was maximized, then minimized, would come
    // back at normal size (#3918).  On a hidden widget this is a pending state
    // that show() applies; on a visible one it takes effect immediately and
    // show() is a no-op.
    w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
    w->show();
    w->raise();
    w->activateWindow();
}

} // namespace AetherSDR
