#pragma once

#include <QGuiApplication>
#include <QMargins>
#include <QWidget>
#include <QWindow>
#include <QtMath>

#ifdef Q_OS_MAC
#include "mac/NativeWindowTitle.h"
#endif

namespace AetherSDR::WindowChrome {

inline bool supportsExpandedClientArea(const QString& platform)
{
    return platform == QStringLiteral("cocoa") || platform == QStringLiteral("windows");
}

inline bool supportsExpandedClientArea()
{
    return supportsExpandedClientArea(QGuiApplication::platformName());
}

// The flags `configure()` applies, as a pure function of the platform so the
// Windows branch can be tested on any host.
inline Qt::WindowFlags chromeFlags(Qt::WindowFlags flags, bool enabled, const QString& platform)
{
    const bool expanded = enabled && supportsExpandedClientArea(platform);
    flags.setFlag(Qt::FramelessWindowHint, enabled && !expanded);
    flags.setFlag(Qt::ExpandedClientAreaHint, expanded);
    flags.setFlag(Qt::NoTitleBarBackgroundHint, expanded);
    if (platform == QStringLiteral("windows")) {
        // Expanded, the bar draws its own caption buttons: Qt 6.12 paints its
        // Windows ones into a layered child window that does not reliably
        // show, coloured from the OS light/dark mode rather than the theme.
        // Without the button hints Qt neither draws nor hit-tests them; the
        // HWND's WS_MINIMIZEBOX/WS_MAXIMIZEBOX are restored natively
        // (MainWindow::applyWindowsCaptionStyles).
        flags.setFlag(Qt::CustomizeWindowHint, enabled);
        flags.setFlag(Qt::WindowTitleHint, !enabled);
        flags.setFlag(Qt::WindowSystemMenuHint);
        flags.setFlag(Qt::WindowMinimizeButtonHint, !enabled);
        flags.setFlag(Qt::WindowMaximizeButtonHint, !enabled);
        flags.setFlag(Qt::WindowCloseButtonHint, !enabled);
    }
    return flags;
}

inline void configure(QWidget* window, bool enabled)
{
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, !enabled);
    window->setAttribute(Qt::WA_TranslucentBackground, false);
    window->setWindowFlags(chromeFlags(window->windowFlags(), enabled,
                                       QGuiApplication::platformName()));
}

// Native caption controls exist only on macOS's expanded client area (the
// traffic lights) and under system decorations; everywhere else the bar's
// own caption buttons are shown.
inline bool usesNativeCaption(Qt::WindowFlags flags, const QString& platform)
{
    if (flags.testFlag(Qt::FramelessWindowHint)) {
        return false;
    }
    return !(flags.testFlag(Qt::ExpandedClientAreaHint) && platform == QStringLiteral("windows"));
}

inline bool usesNativeCaption(const QWidget* window)
{
    return usesNativeCaption(window->windowFlags(), QGuiApplication::platformName());
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
    // Windows: the caption buttons are the bar's own, so only the safe area
    // is reserved.
    return QMargins(safe.left(), 0, safe.right(), safe.bottom());
}

}
