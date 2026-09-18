#pragma once

#include <QVariant>

namespace AetherSDR::SpotCommandPolicy {

inline constexpr const char* kPassiveSpotsModeKey = "PassiveSpotsMode";

bool passiveModeFromSetting(const QVariant& value);
bool passiveSpotsModeEnabled();
bool shouldSendSpotAddCommands(bool backendAlwaysUsesClientSideSpots);

} // namespace AetherSDR::SpotCommandPolicy
