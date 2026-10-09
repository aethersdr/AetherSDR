#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Who put the QT_QPA_PLATFORM value Qt started with in the environment.
enum class QpaRequestSource { Unset, User, AetherSDR };

// The start-up log lines that record which Qt platform plugin is running and
// whether Qt fell back from the first one QT_QPA_PLATFORM asked for. Qt's own
// messages about a plugin that fails to load come before AetherSDR's log
// handler exists, so these lines are what a log can show.
struct QtPlatformChoice {
    static QStringList logLines(const QString& requested, QpaRequestSource source,
                                const QString& platformName);
    // The first plugin in a QT_QPA_PLATFORM value ("wayland;xcb" -> "wayland",
    // "xcb:arg" -> "xcb"); empty when nothing was requested.
    static QString firstRequested(const QString& requested);
    static bool fellBack(const QString& requested, const QString& platformName);
};

} // namespace AetherSDR
