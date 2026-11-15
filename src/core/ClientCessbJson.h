#pragma once

// The final peak control's configuration as one JSON object (Principle V),
// shared by its settings key and TX profiles/presets:
//   {"enabled": bool, "clipDb": num, "cessb": bool, "outputDb": num,
//    "lowHz": num, "highHz": num}

#include <QJsonObject>

namespace AetherSDR {

class ClientCessb;

[[nodiscard]] QJsonObject clientCessbToJson(const ClientCessb& cessb);
// Missing or mistyped keys keep the current value. A passband change takes
// effect when the stage is next prepared.
void applyClientCessbJson(ClientCessb& cessb, const QJsonObject& json);

}  // namespace AetherSDR
