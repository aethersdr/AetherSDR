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
    static constexpr const char* kVersion = "0.5.3";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.5.3/flex-tailnet-shim-0.5.3.tar.gz";
    static constexpr const char* kSha256 =
        "025ab56d1fbae7303608bfa96c9e87a305500360a877ff47dcdbac3c9aacadde";
    static constexpr qint64 kSize = 8085193;
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
    static constexpr const char* kVersion = "0.5.3-native";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "flex-tailnet-shim-v0.5.3/AetherTailnet-0.5.3-native.ssdr_waveform";
    static constexpr const char* kSha256 =
        "14f00925b887e0c0cec0472d8b3dfcae6eb8229fd02307e7734c17dd02697192";
    static constexpr qint64 kSize = 9192923;
};

}  // namespace AetherSDR
