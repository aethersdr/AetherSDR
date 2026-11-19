#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Internal sentinel for commands AetherSDR suppresses before sending to the
// radio during profile recall. This is not a SmartSDR protocol response code.
inline constexpr int kProfileLoadSuppressedCommandCode = 0x50000061;

// Classifies a command as a write to radio state that a profile load owns and
// will rebuild. The backend dispatcher consults this as a final backstop and
// suppresses matches while the profile-load hold is armed.
//
// Callers must not rely on that backstop to swallow user-initiated writes: a
// suppressed command never reaches the wire and is lost. Route pan center /
// bandwidth writes through RadioModel::requestPanCenter(), which defers and
// coalesces them until the hold lifts instead of dropping them (#4142).
inline bool isProfileOwnedRadioStateWrite(const QString& command)
{
    const QString trimmed = command.trimmed();
    const QStringList tokens = trimmed.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() >= 5
        && tokens[0] == QStringLiteral("display")
        && tokens[1] == QStringLiteral("pan")
        && tokens[2] == QStringLiteral("set")) {
        bool hasPixelDimension = false;
        for (int i = 4; i < tokens.size(); ++i) {
            const QString key = tokens[i].section(QLatin1Char('='), 0, 0);
            if (key != QStringLiteral("xpixels") && key != QStringLiteral("ypixels")) {
                return true;
            }
            hasPixelDimension = true;
        }
        // xpixels/ypixels are allowed past this low-level guard because
        // MainWindow queues them during a profile load and flushes them after
        // the radio accepts the profile. Do not bypass
        // MainWindow::requestPanDimensionsForRadio(); early dimension writes
        // can cause the radio to save a partial GUIClient slice layout.
        return !hasPixelDimension;
    }

    return trimmed.startsWith(QStringLiteral("slice set "))
        || trimmed.startsWith(QStringLiteral("display pan set "))
        || trimmed.startsWith(QStringLiteral("display panafall set "))
        || trimmed.startsWith(QStringLiteral("display waterfall set "));
}

} // namespace AetherSDR
