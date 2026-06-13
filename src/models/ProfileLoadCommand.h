#pragma once

#include <QRegularExpression>
#include <QString>

namespace AetherSDR {

struct ProfileLoadCommand {
    bool valid{false};
    QString type;
    QString name;
};

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
