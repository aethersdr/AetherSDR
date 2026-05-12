#pragma once

#include <QMouseEvent>
#include <QPoint>
#include <QWidget>
#include <QWindow>

namespace AetherSDR::FramelessMoveHelper {

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
    if (QWindow* windowHandle = window->windowHandle()) {
        if (windowHandle->startSystemMove()) {
            ev->accept();
            return true;
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
