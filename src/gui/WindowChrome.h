#pragma once

#include <QGuiApplication>
#include <QMargins>
#include <QStyle>
#include <QWidget>
#include <QWindow>
#include <QtMath>

#ifdef Q_OS_MAC
#include "mac/NativeWindowTitle.h"
#endif

namespace AetherSDR::WindowChrome {

inline bool supportsExpandedClientArea()
{
    const QString platform = QGuiApplication::platformName();
    return platform == QStringLiteral("cocoa") || platform == QStringLiteral("windows");
}

inline void configure(QWidget* window, bool enabled)
{
    Qt::WindowFlags flags = window->windowFlags();
    flags.setFlag(Qt::FramelessWindowHint, enabled && !supportsExpandedClientArea());
    flags.setFlag(Qt::ExpandedClientAreaHint, enabled && supportsExpandedClientArea());
    flags.setFlag(Qt::NoTitleBarBackgroundHint, enabled && supportsExpandedClientArea());
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        flags.setFlag(Qt::CustomizeWindowHint, enabled);
        flags.setFlag(Qt::WindowTitleHint, !enabled);
        flags.setFlag(Qt::WindowSystemMenuHint);
        flags.setFlag(Qt::WindowMinimizeButtonHint);
        flags.setFlag(Qt::WindowMaximizeButtonHint);
        flags.setFlag(Qt::WindowCloseButtonHint);
    }
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, !enabled);
    window->setAttribute(Qt::WA_TranslucentBackground, false);
    window->setWindowFlags(flags);
}

inline bool usesNativeCaption(const QWidget* window)
{
    return !window->windowFlags().testFlag(Qt::FramelessWindowHint);
}

inline QMargins contentInsets(const QWidget* window)
{
    const QWindow* handle = window->windowHandle();
    const QMargins safe = handle ? handle->safeAreaMargins() : QMargins();
    if (!window->windowFlags().testFlag(Qt::ExpandedClientAreaHint)) {
        return {};
    }
    if (window->isFullScreen()) {
        return safe;
    }
#ifdef Q_OS_MAC
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        const QRectF controls = mac::nativeCaptionBounds(window);
        return QMargins(qMax(safe.left(), qCeil(controls.right())), 0, safe.right(), safe.bottom());
    }
#endif
    const int titleHeight = qMax(safe.top(),
        window->style()->pixelMetric(QStyle::PM_TitleBarHeight));
    const int controlsWidth = qMax(96, qCeil(titleHeight * 4.5));
    return QMargins(safe.left(), 0, qMax(safe.right(), controlsWidth), safe.bottom());
}

}
