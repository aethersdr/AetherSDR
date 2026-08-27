#include "SpotCommandPolicy.h"

#include "AppSettings.h"

namespace AetherSDR::SpotCommandPolicy {

bool passiveModeFromSetting(const QVariant& value)
{
    return value.toString() == "True";
}

bool passiveSpotsModeEnabled()
{
    return passiveModeFromSetting(
        AppSettings::instance().value(kPassiveSpotsModeKey, "False"));
}

bool shouldSendSpotAddCommands(bool backendAlwaysUsesClientSideSpots)
{
    return !backendAlwaysUsesClientSideSpots && !passiveSpotsModeEnabled();
}

} // namespace AetherSDR::SpotCommandPolicy
