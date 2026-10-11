#pragma once

#include <QtGlobal>

namespace AetherSDR {

// The remote-access container image this AetherSDR release installs (RFC
// #6271 ruling D2). AetherSDR downloads exactly this file and refuses any
// other bytes. The image is reproducible: `tools/flex-tailnet-shim/build.sh`
// at the shim's release tag rebuilds the identical file, so anyone can check
// this hash against the source. The gzip bytes depend on the zlib build, so
// the check needs the toolchain the hash was made with: Go 1.27.1 (go.mod)
// and CPython 3.14 with zlib 1.3.2. build.sh pins the locale itself (from
// 0.4.1; 0.4.0 reproduces only under en_US.UTF-8).
//
// Bump all four together when a new shim release is published.
struct TailnetShimRelease {
    static constexpr const char* kVersion = "0.4.1";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.4.1/flex-tailnet-shim-0.4.1.tar.gz";
    static constexpr const char* kSha256 =
        "ed37c0e3b65a239a3a759e2b030a42be71c69ea4c37bdd149e1bce3f46bed35b";
    static constexpr qint64 kSize = 8055196;
};

}  // namespace AetherSDR
