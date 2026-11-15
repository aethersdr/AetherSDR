#pragma once

// The CFC stage's configuration as one JSON object (Principle V), shared by
// its settings key and by TX profiles and presets so the two cannot drift:
//   {"enabled": bool, "fftSize": int, "precompDb": num, "postEq": bool,
//    "postGainDb": num, "bands": [{"hz": num, "compDb": num, "eqDb": num}]}

#include <QJsonObject>

namespace AetherSDR {

class ClientCfc;

[[nodiscard]] QJsonObject clientCfcToJson(const ClientCfc& cfc);

// Missing or mistyped keys keep the stage's current value; values are
// clamped by the stage's own setters. "bands" applies only when it is an
// array of objects.
void applyClientCfcJson(ClientCfc& cfc, const QJsonObject& json);

}  // namespace AetherSDR
