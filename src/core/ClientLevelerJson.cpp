#include "core/ClientLevelerJson.h"

#include "core/ClientLeveler.h"

#include <QJsonValue>

namespace AetherSDR {

QJsonObject clientLevelerToJson(const ClientLeveler& lev)
{
    return QJsonObject{{"enabled", lev.isEnabled()},
                       {"maxGainDb", static_cast<double>(lev.maxGainDb())},
                       {"attackMs", static_cast<double>(lev.attackMs())},
                       {"decayMs", static_cast<double>(lev.decayMs())},
                       {"hangMs", static_cast<double>(lev.hangMs())}};
}

void applyClientLevelerJson(ClientLeveler& lev, const QJsonObject& json)
{
    const auto apply = [&json](const char* key, auto setter) {
        const QJsonValue v = json.value(QLatin1String(key));
        if (v.isDouble()) {
            setter(static_cast<float>(v.toDouble()));
        }
    };
    apply("maxGainDb", [&lev](float v) { lev.setMaxGainDb(v); });
    apply("attackMs", [&lev](float v) { lev.setAttackMs(v); });
    apply("decayMs", [&lev](float v) { lev.setDecayMs(v); });
    apply("hangMs", [&lev](float v) { lev.setHangMs(v); });
    if (json.value("enabled").isBool()) {
        lev.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
