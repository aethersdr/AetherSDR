#pragma once

#include <QGuiApplication>
#include <QJsonObject>
#include <QMargins>
#include <QOperatingSystemVersion>
#include <QPoint>
#include <QRect>
#include <QString>
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

// Windows 10 only: Qt's expanded client area leaves a resize border on the
// left, right and bottom as non-client, which Windows 10 paints as a light
// strip (#6266), so MainWindow::nativeEvent claims the whole window as client
// area. Windows 11 draws that border invisibly and keeps Qt's answer.
inline bool claimsWholeWindowAsClient(Qt::WindowFlags flags, const QOperatingSystemVersion& os)
{
    return flags.testFlag(Qt::ExpandedClientAreaHint) && os < QOperatingSystemVersion::Windows11;
}

// Windows expanded client area: the part of the window a point is on, for the
// main window's own WM_NCHITTEST. Qt 6.12's answer for these flags turns the
// live mouse-button state into synthetic presses, doubling real clicks (#6272).
// Physical pixels; `border` is the resize band (0 while maximized).
enum class FrameHit { Client, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

inline FrameHit expandedFrameHit(const QPoint& point, const QRect& window, int border)
{
    if (border <= 0 || !window.contains(point)) {
        return FrameHit::Client;
    }
    const bool left = point.x() < window.left() + border;
    const bool right = point.x() > window.right() - border;
    const bool top = point.y() < window.top() + border;
    const bool bottom = point.y() > window.bottom() - border;
    if (left) {
        return top ? FrameHit::TopLeft : bottom ? FrameHit::BottomLeft : FrameHit::Left;
    }
    if (right) {
        return top ? FrameHit::TopRight : bottom ? FrameHit::BottomRight : FrameHit::Right;
    }
    if (top) {
        return FrameHit::Top;
    }
    return bottom ? FrameHit::Bottom : FrameHit::Client;
}

// Windows: the native window rect that gives `client`, keeping the frame the
// window has now (`windowNow` around `clientNow`). All in physical pixels, as
// GetWindowRect / GetClientRect report them. (#6303)
inline QRect windowRectForClient(const QRect& client, const QRect& windowNow, const QRect& clientNow)
{
    const int left = clientNow.x() - windowNow.x();
    const int top = clientNow.y() - windowNow.y();
    const int right = (windowNow.x() + windowNow.width()) - (clientNow.x() + clientNow.width());
    const int bottom = (windowNow.y() + windowNow.height()) - (clientNow.y() + clientNow.height());
    return QRect(client.x() - left, client.y() - top,
                 client.width() + left + right, client.height() + top + bottom);
}

// Windows: the main window's native client rects, one per window role
// ("main", "fullMode", "minimalMode"), each with the device pixel ratio it
// was measured at. Persisted as one object under kNativeGeometryKey
// (Principle V). Reading gives an invalid rect when the role is missing or
// malformed, out of range, or measured at another ratio (UI or display scale
// changed), so the caller falls back to Qt's own restore. (#6303)
inline const QString kNativeGeometryKey = QStringLiteral("MainWindowNativeGeometry");
inline constexpr int kNativeGeometrySchemaVersion = 1;
// Win32 window coordinates are 16-bit signed (GDI); beyond that the saved
// value is corrupt, not a real window.
inline constexpr int kMaxNativeCoordinate = 32767;

inline QJsonObject withNativeClientRect(QJsonObject doc, const QString& role,
                                        const QRect& client, qreal dpr)
{
    doc.insert(QStringLiteral("schemaVersion"), kNativeGeometrySchemaVersion);
    doc.insert(role, QJsonObject{
        {QStringLiteral("x"), client.x()},
        {QStringLiteral("y"), client.y()},
        {QStringLiteral("width"), client.width()},
        {QStringLiteral("height"), client.height()},
        {QStringLiteral("dpr"), dpr},
    });
    return doc;
}

inline QRect savedNativeClientRect(const QJsonObject& doc, const QString& role, qreal dpr)
{
    if (doc.value(QStringLiteral("schemaVersion")).toInt() != kNativeGeometrySchemaVersion) {
        return {};
    }
    const QJsonObject r = doc.value(role).toObject();
    const QJsonValue savedDpr = r.value(QStringLiteral("dpr"));
    if (!savedDpr.isDouble() || !qFuzzyCompare(savedDpr.toDouble(), dpr)) {
        return {};
    }
    int n[4] = {};
    const char* const fields[4] = {"x", "y", "width", "height"};
    for (int i = 0; i < 4; ++i) {
        const QJsonValue v = r.value(QLatin1String(fields[i]));
        const double d = v.toDouble();
        if (!v.isDouble() || d != qFloor(d)
            || d < -kMaxNativeCoordinate || d > kMaxNativeCoordinate) {
            return {};
        }
        n[i] = int(d);
    }
    return QRect(n[0], n[1], n[2], n[3]);  // invalid when empty
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
