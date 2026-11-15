#include "core/ClientCessbJson.h"

#include "core/ClientCessb.h"

#include <QJsonValue>

namespace AetherSDR {

QJsonObject clientCessbToJson(const ClientCessb& c)
{
    return QJsonObject{{"enabled", c.isEnabled()},
                       {"clipDb", static_cast<double>(c.clipDb())},
                       {"cessb", c.overshootControl()},
                       {"outputDb", static_cast<double>(c.outputDb())},
                       {"lowHz", static_cast<double>(c.lowHz())},
                       {"highHz", static_cast<double>(c.highHz())}};
}

void applyClientCessbJson(ClientCessb& c, const QJsonObject& json)
{
    if (json.value("clipDb").isDouble()) {
        c.setClipDb(static_cast<float>(json.value("clipDb").toDouble()));
    }
    if (json.value("outputDb").isDouble()) {
        c.setOutputDb(static_cast<float>(json.value("outputDb").toDouble()));
    }
    if (json.value("cessb").isBool()) {
        c.setOvershootControl(json.value("cessb").toBool());
    }
    if (json.value("lowHz").isDouble() || json.value("highHz").isDouble()) {
        c.setPassband(static_cast<float>(json.value("lowHz").toDouble(c.lowHz())),
                      static_cast<float>(json.value("highHz").toDouble(c.highHz())));
    }
    if (json.value("enabled").isBool()) {
        c.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
