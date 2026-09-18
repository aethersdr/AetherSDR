#pragma once

#include "core/AppSettings.h"
#include "core/discovery/RadioDiscoverySource.h"

namespace AetherSDR::aetherd {

// Called once by the daemon at startup, before constructing model consumers.
// Native discovery reads client-owned Identity nicknames. Use the normal
// settings lifecycle (including failed/read-only protection), but leave passive
// and simulator-only startup independent of the operator's settings store.
[[nodiscard]] inline std::unique_ptr<RadioDiscoverySource> makeDiscoverySource(
    LocalDiscoveryOptions options)
{
    if (options.local) {
        AppSettings::instance().load();
    }
    return makeLocalRadioDiscoverySource(options);
}

} // namespace AetherSDR::aetherd
