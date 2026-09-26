#include "gui/WindowShowState.h"

#include <QApplication>
#include <QHash>
#include <QMetaObject>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace AetherSDR {

namespace {

// Qt::Desktop is deprecated in Qt 6, but its numeric window type can still
// appear in topLevelWidgets() on older platform plugins. Keep the filter
// without naming the deprecated enumerator (and therefore without adding a
// warning to every build).
constexpr Qt::WindowType kDesktopWindowType =
    static_cast<Qt::WindowType>(static_cast<int>(Qt::Window) | 0x10);

bool isUserFacingTopLevelWindow(const QWidget* w)
{
    return w && w->isWindow() && w->isVisible()
        && windowTypeAppearsInMenu(w->windowType());
}

} // namespace

bool windowTypeAppearsInMenu(Qt::WindowType type)
{
    switch (type) {
    case Qt::Popup:
    case Qt::ToolTip:
    case Qt::SplashScreen:
    case kDesktopWindowType:
    case Qt::SubWindow:
        return false;
    default:
        return true;
    }
}

bool windowIsShowing(const QWidget* w)
{
    return w && w->isVisible() && !w->isMinimized();
}

void showAndRaiseWindow(QWidget* w)
{
    if (!w) {
        return;
    }
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

QString windowMenuLabel(const QWidget* w)
{
    if (!w) {
        return {};
    }

    QString title = w->windowTitle();
    title.remove(QStringLiteral("[*]"));
    title = title.trimmed();
    if (!title.isEmpty()) {
        return title;
    }

    const QString objectName = w->objectName().trimmed();
    if (!objectName.isEmpty()) {
        return objectName;
    }

    return QString::fromLatin1(w->metaObject()->className());
}

QList<WindowMenuEntry> windowInventory(const QWidget* primaryWindow)
{
    return windowInventory(QApplication::topLevelWidgets(), primaryWindow);
}

QString windowMenuText(QString title)
{
    return title.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

QList<WindowMenuEntry> windowInventory(const QList<QWidget*>& candidates,
                                      const QWidget* primaryWindow)
{
    QList<QWidget*> windows;
    for (QWidget* w : candidates) {
        if (isUserFacingTopLevelWindow(w)) {
            windows.append(w);
        }
    }

    std::sort(windows.begin(), windows.end(),
              [primaryWindow](const QWidget* lhs, const QWidget* rhs) {
        if (lhs == primaryWindow || rhs == primaryWindow) {
            return lhs == primaryWindow && rhs != primaryWindow;
        }

        const int titleOrder = QString::localeAwareCompare(
            windowMenuLabel(lhs), windowMenuLabel(rhs));
        if (titleOrder != 0) {
            return titleOrder < 0;
        }

        const int objectOrder = QString::localeAwareCompare(
            lhs->objectName(), rhs->objectName());
        if (objectOrder != 0) {
            return objectOrder < 0;
        }

        const int classOrder = QString::localeAwareCompare(
            QString::fromLatin1(lhs->metaObject()->className()),
            QString::fromLatin1(rhs->metaObject()->className()));
        if (classOrder != 0) {
            return classOrder < 0;
        }

        // Address is stable for the life of a window, so duplicate numbering
        // cannot shuffle between aboutToShow rebuilds.
        return std::less<const QWidget*>{}(lhs, rhs);
    });

    QHash<QString, int> titleCounts;
    for (const QWidget* window : windows) {
        ++titleCounts[windowMenuLabel(window)];
    }
    QHash<QString, int> titleOccurrences;

    QList<WindowMenuEntry> entries;
    entries.reserve(windows.size());
    for (QWidget* window : windows) {
        const QString title = windowMenuLabel(window);
        QString menuText = title;
        if (titleCounts.value(title) > 1) {
            menuText = QStringLiteral("%1 (%2)")
                           .arg(title)
                           .arg(++titleOccurrences[title]);
        }
        if (window->isMinimized()) {
            menuText = QStringLiteral("%1 — Minimized").arg(menuText);
        }
        // QAction text treats '&' as a mnemonic marker. Window titles are
        // operator data (canvas names are free-form), so escape after all
        // suffixes have been applied.
        entries.append({window, title, windowMenuText(menuText)});
    }
    return entries;
}

} // namespace AetherSDR
