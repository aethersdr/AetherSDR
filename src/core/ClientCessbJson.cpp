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
                       {"route", c.route() == ClientCessb::Route::Modulator
                                     ? QStringLiteral("modulator")
                                     : QStringLiteral("chain")}};
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
    if (json.value("route").isString()) {
        // Anything but "modulator" is the chain.
        c.setRoute(json.value("route").toString() == QLatin1String("modulator")
                       ? ClientCessb::Route::Modulator
                       : ClientCessb::Route::Chain);
    }
    if (json.value("enabled").isBool()) {
        c.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
