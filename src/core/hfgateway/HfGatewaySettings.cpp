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

void Settings::save(const Config& config)
{
    auto& s = AppSettings::instance();
    s.setValue(QString::fromLatin1(kRootKey),
               QString::fromUtf8(QJsonDocument(toJson(config)).toJson(QJsonDocument::Compact)));
    s.save();
}

}  // namespace AetherSDR::HfGateway
