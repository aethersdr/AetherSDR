#include "core/ClientCessbJson.h"

#include "core/ClientCessb.h"

#include <QJsonValue>

namespace AetherSDR {

QJsonObject clientCessbToJson(const ClientCessb& c)
{
    return QJsonObject{{"enabled", c.isEnabled()},
                       {"clipDb", static_cast<double>(c.clipDb())},
                       {"cessb", c.overshootControl()},
                       {"outputDb", static_cast<double>(c.outputDb())}};
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
    if (json.value("enabled").isBool()) {
        c.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
