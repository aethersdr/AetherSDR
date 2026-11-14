#pragma once

#include "core/hfgateway/HfGatewayConfig.h"

// The HF Gateway's stored configuration: one JSON object under the root key
// "HfGateway" in AppSettings (Principle V), read through HfGateway::fromJson()
// and always written whole (Principle XIV). Process-wide, client-side.
//
// The transmit arm is not part of this and is never stored.

namespace AetherSDR::HfGateway {

class Settings {
public:
    static constexpr const char* kRootKey = "HfGateway";

    static Config load();
    static void save(const Config& config);
};

}  // namespace AetherSDR::HfGateway
