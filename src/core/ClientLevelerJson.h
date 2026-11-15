#pragma once

// The LVLR stage's configuration as one JSON object (Principle V), shared by
// its settings key and by TX profiles and presets:
//   {"enabled": bool, "maxGainDb": num, "attackMs": num, "decayMs": num,
//    "hangMs": num}

#include <QJsonObject>

namespace AetherSDR {

class ClientLeveler;

[[nodiscard]] QJsonObject clientLevelerToJson(const ClientLeveler& lev);

// Missing or mistyped keys keep the stage's current value; the stage's own
// setters clamp the rest.
void applyClientLevelerJson(ClientLeveler& lev, const QJsonObject& json);

}  // namespace AetherSDR
