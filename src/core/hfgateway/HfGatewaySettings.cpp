#include "core/hfgateway/HfGatewaySettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>

namespace AetherSDR::HfGateway {

Config Settings::load()
{
    const QString json = AppSettings::instance().value(QString::fromLatin1(kRootKey), QString{}).toString();
    if (json.isEmpty()) {
        return {};
    }
    return fromJson(QJsonDocument::fromJson(json.toUtf8()).object());
}

bool Settings::save(const Config& config)
{
    auto& settings = AppSettings::instance();
    if (!settings.storeWritable()) {
        return false;
    }

    const QString key = QString::fromLatin1(kRootKey);
    const QVariant previous = settings.value(key);
    const QJsonObject oldDocument = QJsonDocument::fromJson(previous.toString().toUtf8()).object();
    if (oldDocument.value(QStringLiteral("version")).toDouble() > Config::kSchemaVersion) {
        return false;
    }

    const QString json = QString::fromUtf8(QJsonDocument(toJson(config)).toJson(QJsonDocument::Compact));
    settings.setValue(key, json);
    settings.save();

    QString onDisk;
    if (settings.readAppRowFromDisk(key, onDisk) && onDisk == json) {
        return true;
    }

    // AppSettings retains dirty values for retry. A failed edit should not
    // surface in the UI now or be committed by an unrelated later save.
    if (previous.isValid()) {
        settings.setValue(key, previous);
    } else {
        settings.remove(key);
    }
    return false;
}

}  // namespace AetherSDR::HfGateway
