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
    static constexpr const char* kVersion = "0.3.2";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.3.2/flex-tailnet-shim-0.3.2.tar.gz";
    static constexpr const char* kSha256 =
        "98ece95e9a5fbf15f10d38c776eec2bbcb329cc3af26e6cd98e62aa9f6d868ae";
    static constexpr qint64 kSize = 8027580;
};

}  // namespace AetherSDR
