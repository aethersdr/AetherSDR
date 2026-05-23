#include "SpotCommandPolicy.h"

#include "AppSettings.h"

namespace MasterSDR::SpotCommandPolicy {

bool passiveModeFromSetting(const QVariant& value)
{
    return value.toString() == "True";
}

bool passiveSpotsModeEnabled()
{
    return passiveModeFromSetting(
        AppSettings::instance().value(kPassiveSpotsModeKey, "False"));
}

bool shouldSendSpotAddCommands()
{
    return !passiveSpotsModeEnabled();
}

} // namespace MasterSDR::SpotCommandPolicy
