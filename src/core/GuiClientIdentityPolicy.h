#pragma once

#include <QRegularExpression>
#include <QString>
#include <QUuid>

namespace AetherSDR::GuiClientIdentityPolicy {

inline QString automationClientId(const QString& identity)
{
    static const QUuid kNamespace(
        QStringLiteral("8bc98e90-4e5a-5e62-8a95-9b975276a987"));
    return QUuid::createUuidV5(kNamespace, identity.toUtf8())
        .toString(QUuid::WithoutBraces).toUpper();
}

inline QString protocolSafeStation(QString value)
{
    value = value.trimmed();
    value.replace(QRegularExpression(QStringLiteral("[\\s|=\\x00-\\x1F\\x7F]+")),
                  QStringLiteral("-"));
    // Truncate before the trailing-dash strip so a cut that lands right after a
    // dash cannot leave the result ending in one.
    value = value.left(48);
    value.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return value;
}

inline QString automationAgentName(QString configuredName,
                                   QString legacyStation,
                                   QString label)
{
    configuredName = configuredName.trimmed();
    if (!configuredName.isEmpty()) {
        return configuredName;
    }

    legacyStation = legacyStation.trimmed();
    if (!legacyStation.isEmpty()) {
        return legacyStation;
    }

    label = label.trimmed();
    if (!label.isEmpty()) {
        return label;
    }

    return QStringLiteral("Automation");
}

inline bool shouldSelectDistinctId(bool transientIdentity,
                                   const QString& ourStation,
                                   const QString& otherStation,
                                   bool knownPredecessor)
{
    if (transientIdentity) {
        return true;
    }
    return !knownPredecessor || otherStation.isEmpty()
           || otherStation.compare(ourStation, Qt::CaseInsensitive) != 0;
}

} // namespace AetherSDR::GuiClientIdentityPolicy
