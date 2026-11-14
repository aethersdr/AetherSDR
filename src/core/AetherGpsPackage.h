#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// The AetherGPS relay (tools/aether-gps), which shares the radio's gpsd on
// the LAN: TCP 2947 (gpsd protocol, read-only) and UDP 10110 (NMEA). It ships
// inside AetherSDR as Qt resources, a package for each kind of radio, and is
// installed from AetherGPS's "Install AetherGPS on the radio" checkbox only if
// the bytes match the pin. The Waveforms list hides it: the checkbox is its
// only control, as for the radio health tools. `tools/aether-gps/build.py`
// rebuilds both identical files with zig 0.17.0; copy them to
// resources/waveforms/ and bump all of this together.
struct AetherGpsPackage {
    struct Pin {
        const char* resource;
        const char* fileName;
        const char* sha256;
        qint64 size;
    };
    static constexpr const char* kVersion = "0.1.0";
    // FLEX-6000: a legacy waveform (ARMv7).
    static constexpr Pin kLegacy{":/waveforms/AetherGPS.ssdr_waveform", "AetherGPS.ssdr_waveform",
                                 "7e371bc82163061bb956abb719f21debcd7faace16f81df5d4103b2ee0098fe9", 35207};
    // FLEX-8000 and Aurora: a container (arm64) on the radio's own network.
    static constexpr Pin kContainer{":/waveforms/AetherGPS.tar.gz", "AetherGPS.tar.gz",
                                    "dcdbf3447d6f20194f2663a8718ceeec88dd1df7b761755d9bdbd47cf29d3bce", 42873};
};

// The waveform's name, in both packages.
inline constexpr const char* kAetherGpsWaveformName = "AetherGPS";

// True for the relay's row in the radio's installed list, either package.
inline bool isAetherGpsWaveform(const QString& name)
{
    return name == QLatin1String(kAetherGpsWaveformName);
}

namespace aethergps {

// Writes a bundled package to `dir` for the installer, reusing a matching
// copy already there. Only bytes matching the pin are written: empty, with
// `error` set, otherwise.
QString writeBundledPackage(const AetherGpsPackage::Pin& pin, const QString& dir, QString* error);

}  // namespace aethergps

}  // namespace AetherSDR
