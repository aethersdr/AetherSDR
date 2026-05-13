#include "AntennaAliasStore.h"
#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

QString encodedKeyPart(const QString& value)
{
    const QByteArray hex = value.toUtf8().toHex();
    return hex.isEmpty() ? QStringLiteral("default") : QString::fromLatin1(hex);
}

} // namespace

QString AntennaAliasStore::settingsKeyForRadio(const QString& radioKey)
{
    return QStringLiteral("AntennaAliases_%1").arg(encodedKeyPart(radioKey));
}

QMap<QString, QString> AntennaAliasStore::load(const QString& radioKey)
{
    QMap<QString, QString> aliases;
    const QString raw = AppSettings::instance()
        .value(settingsKeyForRadio(radioKey), QString()).toString();
    if (raw.trimmed().isEmpty())
        return aliases;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return aliases;

    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QString alias = it.value().toString().trimmed();
        if (!alias.isEmpty())
            aliases.insert(it.key(), alias);
    }
    return aliases;
}

void AntennaAliasStore::save(const QString& radioKey,
                             const QMap<QString, QString>& aliases)
{
    QJsonObject obj;
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
        const QString alias = it.value().trimmed();
        if (!it.key().isEmpty() && !alias.isEmpty())
            obj.insert(it.key(), alias);
    }

    auto& settings = AppSettings::instance();
    const QString key = settingsKeyForRadio(radioKey);
    if (obj.isEmpty()) {
        settings.remove(key);
    } else {
        settings.setValue(key, QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }
    settings.save();
}

QString AntennaAliasStore::alias(const QMap<QString, QString>& aliases,
                                 const QString& token)
{
    return aliases.value(token).trimmed();
}

QString AntennaAliasStore::displayName(const QMap<QString, QString>& aliases,
                                       const QString& token,
                                       bool includeTokenForDisambiguation)
{
    const QString a = alias(aliases, token);
    if (a.isEmpty())
        return token;
    if (includeTokenForDisambiguation)
        return QStringLiteral("%1 (%2)").arg(a, token);
    return a;
}

QString AntennaAliasStore::shortDisplayName(const QMap<QString, QString>& aliases,
                                            const QString& token,
                                            int maxChars)
{
    const QString label = displayName(aliases, token);
    if (maxChars > 0 && label.size() > maxChars)
        return label.left(maxChars);
    return label;
}

} // namespace AetherSDR
