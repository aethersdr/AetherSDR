#pragma once

// The final peak control's configuration as one JSON object (Principle V),
// shared by its settings key and TX profiles/presets:
//   {"enabled": bool, "clipDb": num, "cessb": bool, "outputDb": num}
// The passband is not a setting: it follows the radio's TX filter.

#include <QJsonObject>

namespace AetherSDR {

class ClientCessb;

[[nodiscard]] QJsonObject clientCessbToJson(const ClientCessb& cessb);
// Missing or mistyped keys keep the current value.
void applyClientCessbJson(ClientCessb& cessb, const QJsonObject& json);

}  // namespace AetherSDR
