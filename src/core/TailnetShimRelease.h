#pragma once

#include <QtGlobal>

namespace AetherSDR {

// The remote-access container image this AetherSDR release installs (RFC
// #6271 ruling D2). AetherSDR downloads exactly this file and refuses any
// other bytes. The image is reproducible: `tools/flex-tailnet-shim/build.sh`
// at the shim's release tag rebuilds the identical file, so anyone can check
// this hash against the source.
//
// Bump all four together when a new shim release is published.
struct TailnetShimRelease {
    static constexpr const char* kVersion = "0.4.0";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.4.0/flex-tailnet-shim-0.4.0.tar.gz";
    static constexpr const char* kSha256 =
        "f79f384e0bab6f4a4351ad3a2790d159c020030077a48c2c64985856320771e2";
    static constexpr qint64 kSize = 8038202;
};

}  // namespace AetherSDR
