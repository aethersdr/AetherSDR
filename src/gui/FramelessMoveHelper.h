#pragma once

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QWidget>
#include <QWindow>

namespace AetherSDR::FramelessMoveHelper {

// QWindow::startSystemMove()/startSystemResize() report success on the xcb
// platform, but the WM-driven interactive move/resize grab they hand off to
// is unreliable there (upstream QTBUG-69716) — under at least Mutter/XWayland
// (the common case for `QT_QPA_PLATFORM=xcb` on a Wayland desktop — the
// workaround for the native-Wayland panadapter stall, #4725) the request is
// silently dropped: the press is consumed, but the window never
// actually tracks the pointer (#4827). Qt's own QSizeGrip hits the same wall
// and works around it in usePlatformSizeGrip() (qsizegrip.cpp) by refusing
// the platform path on xcb and dragging the geometry manually instead —
// every frameless move/resize path in AetherSDR follows that same rule,
// clause for clause: usePlatformSizeGrip() also refuses the platform path on
// Windows when the top-level has Qt::WA_TranslucentBackground set
// (QTBUG-90628 — flicker combining native resize with a translucent
// top-level there), which AetherSDR's own MainWindow sets whenever frameless
// mode is on (`setAttribute(Qt::WA_TranslucentBackground, true)` in
// MainWindow.cpp). `window` may be null for a caller with no widget handy;
// the Windows clause then simply doesn't apply, same as if the attribute
// were unset.
inline bool systemMoveResizeUnreliable(const QWidget* window)
{
    const QString platform = QGuiApplication::platformName();
    if (platform.contains(QLatin1String("xcb"))) {
        return true;
    }
    if (window && window->testAttribute(Qt::WA_TranslucentBackground)
        && platform == QLatin1String("windows")) {
        return true;
    }
    return false;
}

namespace Detail {

constexpr const char* kActiveProperty = "_aetherManualWindowMoveActive";
constexpr const char* kPressGlobalProperty = "_aetherManualWindowMovePressGlobal";
constexpr const char* kStartPosProperty = "_aetherManualWindowMoveStartPos";

inline void startManualMove(QWidget* handle, QWidget* window, QMouseEvent* ev)
{
    handle->setProperty(kActiveProperty, true);
    handle->setProperty(kPressGlobalProperty, ev->globalPosition().toPoint());
    handle->setProperty(kStartPosProperty, window->pos());
    handle->grabMouse();
    ev->accept();
}

inline bool manualMoveActive(QWidget* handle)
{
    return handle && handle->property(kActiveProperty).toBool();
}

} // namespace Detail

inline bool start(QWidget* handle, QMouseEvent* ev)
{
    if (!handle || !ev || ev->button() != Qt::LeftButton) {
        return false;
    }

    QWidget* window = handle->window();
    if (!window) {
        return false;
    }

#ifndef Q_OS_MAC
    if (!systemMoveResizeUnreliable(window)) {
        if (QWindow* windowHandle = window->windowHandle()) {
            if (windowHandle->startSystemMove()) {
                ev->accept();
                return true;
            }
        }
    }
#endif

    Detail::startManualMove(handle, window, ev);
    return true;
}

inline bool finish(QWidget* handle, QMouseEvent* ev)
{
    if (!Detail::manualMoveActive(handle)) {
        return false;
    }

    handle->setProperty(Detail::kActiveProperty, false);
    handle->releaseMouse();
    if (ev) {
        ev->accept();
    }
    return true;
}

inline bool move(QWidget* handle, QMouseEvent* ev)
{
    if (!Detail::manualMoveActive(handle) || !ev) {
        return false;
    }

    if (!(ev->buttons() & Qt::LeftButton)) {
        return finish(handle, ev);
    }

    QWidget* window = handle->window();
    if (window) {
        const QPoint pressGlobal =
            handle->property(Detail::kPressGlobalProperty).toPoint();
        const QPoint startPos =
            handle->property(Detail::kStartPosProperty).toPoint();
        window->move(startPos + ev->globalPosition().toPoint() - pressGlobal);
    }

    ev->accept();
    return true;
}

inline void toggleMaximized(QWidget* handle)
{
    if (!handle) {
        return;
    }

    QWidget* window = handle->window();
    if (!window) {
        return;
    }

    if (window->isMaximized()) {
        window->showNormal();
    } else {
        window->showMaximized();
    }
}

} // namespace AetherSDR::FramelessMoveHelper
