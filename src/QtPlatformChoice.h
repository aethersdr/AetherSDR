#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// Who made the platform request Qt started with: the -platform argument, which
// Qt gives precedence over the environment, or QT_QPA_PLATFORM and who put it
// in the environment.
enum class QpaRequestSource { Unset, User, AetherSDR, CommandLine };

// The start-up log lines that record which Qt platform plugin is running and
// whether Qt fell back from the first one requested. Qt's own messages about a
// plugin that fails to load come before AetherSDR's log handler exists, so
// these lines are what a log can show.
struct QtPlatformChoice {
    // `requested` is the effective request: the -platform value when source is
    // CommandLine, otherwise QT_QPA_PLATFORM's.
    static QStringList logLines(const QString& requested, QpaRequestSource source,
                                const QString& platformName);
    // The first plugin in a request ("wayland;xcb" -> "wayland",
    // "xcb:arg" -> "xcb"); empty when nothing was requested.
    static QString firstRequested(const QString& requested);
    static bool fellBack(const QString& requested, const QString& platformName);
    // The value of the -platform (or --platform) argument as Qt reads it: the
    // next argument, the last occurrence winning. Read before QApplication,
    // which removes the arguments it consumes.
    static std::optional<QString> platformArgument(int argc, const char* const* argv);
};

} // namespace AetherSDR
