#include "CrossNeedleMeterSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR::CrossNeedleMeterSettingsCodec {

QString normalizeTheme(const QString& theme)
{
    if (theme == kClassicTheme || theme == kDarkTheme) {
        return theme;
    }
    return kUplightTheme;
}

QString encode(const Snapshot& settings)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("faceTheme"), normalizeTheme(settings.faceTheme));
    root.insert(QStringLiteral("showRange"), settings.showRange);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Snapshot decode(const QByteArray& encoded, QString* error)
{
    if (error) {
        error->clear();
    }

    Snapshot settings;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("root is not an object");
        }
        return settings;
    }

    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    if (version != 1 && version != kVersion) {
        if (error) {
            *error = QStringLiteral("unsupported CrossNeedleMeter settings version");
        }
        return settings;
    }

    settings.faceTheme = normalizeTheme(
        root.value(QStringLiteral("faceTheme")).toString());
    if (version >= 2) {
        settings.showRange =
            root.value(QStringLiteral("showRange")).toBool(true);
    }
    return settings;
}

Snapshot migrateLegacyTheme(const QString& theme)
{
    Snapshot settings;
    settings.faceTheme = normalizeTheme(theme);
    return settings;
}

} // namespace AetherSDR::CrossNeedleMeterSettingsCodec
