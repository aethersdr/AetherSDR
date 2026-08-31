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
    // showNormal() only for a minimized window — see the header note on #3918.
    if (w->isMinimized())
        w->showNormal();
    else
        w->show();
    w->raise();
    w->activateWindow();
}

} // namespace AetherSDR
