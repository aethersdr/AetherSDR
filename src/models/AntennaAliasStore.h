#pragma once

#include <QMap>
#include <QString>

namespace AetherSDR {

class AntennaAliasStore {
public:
    static QString settingsKeyForRadio(const QString& radioKey);
    static QMap<QString, QString> load(const QString& radioKey);
    static void save(const QString& radioKey, const QMap<QString, QString>& aliases);

    static QString alias(const QMap<QString, QString>& aliases,
                         const QString& token);
    static QString displayName(const QMap<QString, QString>& aliases,
                               const QString& token,
                               bool includeTokenForDisambiguation = false);
    static QString shortDisplayName(const QMap<QString, QString>& aliases,
                                    const QString& token,
                                    int maxChars = 6);
};

} // namespace AetherSDR
