#pragma once

// The multiband limiter's configuration as one JSON object (Principle V),
// shared by its settings key and TX profiles/presets:
//   {"enabled": bool, "x1": num, "x2": num, "x3": num,
//    "bands": [{"thresholdDb": num, "releaseMs": num} x 4]}

#include <QJsonObject>

namespace AetherSDR {

class ClientMbLimiter;

[[nodiscard]] QJsonObject clientMbLimiterToJson(const ClientMbLimiter& limiter);
// Missing or mistyped keys keep the current value.
void applyClientMbLimiterJson(ClientMbLimiter& limiter, const QJsonObject& json);

}  // namespace AetherSDR
