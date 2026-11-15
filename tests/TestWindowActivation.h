#pragma once

#include <QApplication>
#include <QWidget>

// Make `w` the active window, so a test can exercise code paths gated on
// focus.
//
// QWidget::activateWindow() is what Qt's own deprecation notice points you at,
// and under the headless platform plugins it does nothing whatsoever: the
// window never becomes active, QWidget::hasFocus() never returns true no
// matter how many times setFocus() is called, and anything guarded by focus
// stays correctly silent. Measured on Qt 6.11 under offscreen:
//
//   after activateWindow():   isActiveWindow=0  hasFocus=0  focusWidget=(nil)
//   after setActiveWindow():  isActiveWindow=1  hasFocus=1
//
// QApplication::setActiveWindow() is deprecated in Qt 6.7+, not removed, and
// nothing supported replaces it for a test that has to establish activation
// with no window manager to ask. So: try the supported call, and fall back
// only when the platform declined to activate.
//
// This is why the a11y announcement tests read `got []` rather than an
// announcement — the widgets were right to stay quiet, the fixture had simply
// never given them focus. Wayland declines self-activation too, so those tests
// failed on a real display for the same reason.
inline void activateForTest(QWidget& w)
{
    w.activateWindow();
    if (w.isActiveWindow()) {
        return;
    }
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&w);
QT_WARNING_POP
}
