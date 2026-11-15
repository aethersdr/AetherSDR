#include "core/ClientMbLimiterJson.h"

#include "core/ClientMbLimiter.h"

#include <QJsonArray>
#include <QJsonValue>

namespace AetherSDR {

QJsonObject clientMbLimiterToJson(const ClientMbLimiter& l)
{
    QJsonArray bands;
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        bands.append(QJsonObject{{"thresholdDb", static_cast<double>(l.thresholdDb(b))},
                                 {"releaseMs", static_cast<double>(l.releaseMs(b))}});
    }
    return QJsonObject{{"enabled", l.isEnabled()},
                       {"x1", static_cast<double>(l.crossoverHz(0))},
                       {"x2", static_cast<double>(l.crossoverHz(1))},
                       {"x3", static_cast<double>(l.crossoverHz(2))},
                       {"bands", bands}};
}

void applyClientMbLimiterJson(ClientMbLimiter& l, const QJsonObject& json)
{
    const auto number = [&json](const char* key, float current) {
        const QJsonValue v = json.value(QLatin1String(key));
        return v.isDouble() ? static_cast<float>(v.toDouble()) : current;
    };
    l.setCrossoversHz(number("x1", l.crossoverHz(0)), number("x2", l.crossoverHz(1)),
                      number("x3", l.crossoverHz(2)));
    const QJsonArray bands = json.value("bands").toArray();
    for (int b = 0; b < ClientMbLimiter::kBands && b < bands.size(); ++b) {
        const QJsonObject band = bands.at(b).toObject();
        if (band.value("thresholdDb").isDouble()) {
            l.setThresholdDb(b, static_cast<float>(band.value("thresholdDb").toDouble()));
        }
        if (band.value("releaseMs").isDouble()) {
            l.setReleaseMs(b, static_cast<float>(band.value("releaseMs").toDouble()));
        }
    }
    if (json.value("enabled").isBool()) {
        l.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
