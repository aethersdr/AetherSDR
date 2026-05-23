#pragma once

#include <QVariant>

namespace MasterSDR::SpotCommandPolicy {

inline constexpr const char* kPassiveSpotsModeKey = "PassiveSpotsMode";

bool passiveModeFromSetting(const QVariant& value);
bool passiveSpotsModeEnabled();
bool shouldSendSpotAddCommands();

} // namespace MasterSDR::SpotCommandPolicy
