#pragma once

#include <QLatin1Char>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace AetherSDR {

struct ProfileLoadCommand {
    bool valid{false};
    QString type;
    QString name;
};

inline constexpr qint64 kProfileLoadStateWriteHoldMs = 10000;
inline constexpr int kProfileLoadDeferredPanFlushDelayMs =
    static_cast<int>(kProfileLoadStateWriteHoldMs + 1000);
inline constexpr int kProfileLoadPostHoldRecoveryDelayMs =
    static_cast<int>(kProfileLoadStateWriteHoldMs + 1250);

// Internal sentinel for commands AetherSDR suppresses before sending to the
// radio during profile recall. This is not a SmartSDR protocol response code.
inline constexpr int kProfileLoadSuppressedCommandCode = 0x50000061;

inline bool profileLoadMayRebuildRadioTopology(const QString& profileType)
{
    return profileType == QStringLiteral("global");
}

inline ProfileLoadCommand parseProfileLoadCommand(const QString& command)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*profile\\s+(global|tx|mic)\\s+load\\s+\"([^\"]*)\"\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(command);
    if (!match.hasMatch()) {
        return {};
    }

    return {
        true,
        match.captured(1).toLower(),
        match.captured(2),
    };
}

// Classifies a command as a write to radio state that a profile load owns and
// will rebuild. RadioModel::sendCmd() consults this as a low-level backstop and
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
