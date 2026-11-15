#include "core/ClientCfcJson.h"

#include "core/ClientCfc.h"

#include <QJsonArray>
#include <QJsonValue>

#include <vector>

namespace AetherSDR {

namespace {

float number(const QJsonObject& o, const char* key, float fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<float>(v.toDouble()) : fallback;
}

}  // namespace

QJsonObject clientCfcToJson(const ClientCfc& cfc)
{
    QJsonArray bands;
    for (const ClientCfc::Band& b : cfc.bands()) {
        bands.append(QJsonObject{{"hz", static_cast<double>(b.hz)},
                                 {"compDb", static_cast<double>(b.compDb)},
                                 {"eqDb", static_cast<double>(b.eqDb)}});
    }
    return QJsonObject{{"enabled", cfc.isEnabled()},
                       {"fftSize", cfc.fftSize()},
                       {"precompDb", static_cast<double>(cfc.precompDb())},
                       {"postEq", cfc.postEqEnabled()},
                       {"postGainDb", static_cast<double>(cfc.postGainDb())},
                       {"bands", bands}};
}

void applyClientCfcJson(ClientCfc& cfc, const QJsonObject& json)
{
    if (json.value("fftSize").isDouble()) {
        cfc.setFftSize(json.value("fftSize").toInt(cfc.fftSize()));
    }
    cfc.setPrecompDb(number(json, "precompDb", cfc.precompDb()));
    cfc.setPostGainDb(number(json, "postGainDb", cfc.postGainDb()));
    if (json.value("postEq").isBool()) {
        cfc.setPostEqEnabled(json.value("postEq").toBool());
    }
    if (json.value("bands").isArray()) {
        std::vector<ClientCfc::Band> bands;
        for (const QJsonValue& v : json.value("bands").toArray()) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject b = v.toObject();
            bands.push_back({number(b, "hz", 1000.0f), number(b, "compDb", 0.0f),
                             number(b, "eqDb", 0.0f)});
        }
        cfc.setBands(std::move(bands));
    }
    if (json.value("enabled").isBool()) {
        cfc.setEnabled(json.value("enabled").toBool());
    }
}

}  // namespace AetherSDR
