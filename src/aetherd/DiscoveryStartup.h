#pragma once

#include "core/AppSettings.h"
#include "core/discovery/RadioDiscoverySource.h"

namespace AetherSDR::aetherd {

[[nodiscard]] inline bool loadSmartLinkSettings()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();
    return settings.storeReadable();
}

// Called once by the daemon at startup, before constructing model consumers.
// Native discovery reads client-owned Identity nicknames. Use the normal
// settings lifecycle (including failed/read-only protection), but leave passive
// and simulator-only startup independent of the operator's settings store.
[[nodiscard]] inline std::unique_ptr<RadioDiscoverySource> makeDiscoverySource(
    LocalDiscoveryOptions options, bool smartLink = false)
{
    if (smartLink) {
        if (!loadSmartLinkSettings()) { return {}; }
    } else if (options.local) {
        AppSettings::instance().load();
    }
    return makeLocalRadioDiscoverySource(options);
}

} // namespace AetherSDR::aetherd
