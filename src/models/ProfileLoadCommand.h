#pragma once

#include <QRegularExpression>
#include <QString>
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

} // namespace AetherSDR
