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
// Bump all four together when a new shim release is published. Each pin
// reproduces from the tag in its own URL, not from the tree around it: the
// source's shimVersion runs ahead of a pin until that release is cut.
struct TailnetShimRelease {
    static constexpr const char* kVersion = "0.5.2";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.5.2/flex-tailnet-shim-0.5.2.tar.gz";
    static constexpr const char* kSha256 =
        "a12646ec3fa9665add9a30b33e4ae4846d1f98b5c5d6d786627f09342d699c75";
    static constexpr qint64 kSize = 8082988;
};

// Which of the two shim packages a radio takes.
enum class TailnetShimPackage {
    Container,   // a Docker image, for a radio with a Waveform Processor
    Native,      // a legacy .ssdr_waveform, for one without
};

// The same shim as a legacy waveform, for radios with no Waveform Processor
// (the FLEX-6000 series), which install .ssdr_waveform packages instead of
// containers. `tools/flex-tailnet-shim/native/build.py` at the release tag
// rebuilds the identical file with the same toolchain.
struct TailnetShimNativeRelease {
    static constexpr const char* kVersion = "0.5.2-native";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.5.2/AetherTailnet-0.5.2-native.ssdr_waveform";
    static constexpr const char* kSha256 =
        "099f01a81d22d3cb3b479a6d012da9d78698dabe7729a244ad7b46359a4e2f54";
    static constexpr qint64 kSize = 9190664;
};

}  // namespace AetherSDR
