#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// The radio health agent ("Install radio health tools" in the Waveforms
// window): a legacy waveform that reports the radio's free storage, memory
// and CPU on the LAN. It ships inside AetherSDR as a Qt resource (about
// 63 KB), and is installed only if its bytes match this pin.
// `tools/radio-health/build.py` rebuilds the identical file with zig 0.17;
// copy it to resources/waveforms/ and bump all four together.
struct RadioHealthPackage {
    static constexpr const char* kVersion = "0.1.0";
    static constexpr const char* kResource = ":/waveforms/AetherHealth.ssdr_waveform";
    static constexpr const char* kSha256 =
        "68792b711f09a0a4d5dbf6b614da3bcb588c23c9a9972a8b59473906988a84bc";
    static constexpr qint64 kSize = 63098;
};

// The agent's waveform name (tools/radio-health/package/AetherHealth.cfg).
// The Waveforms list hides it: the checkbox is its only control.
inline constexpr const char* kRadioHealthWaveformName = "AetherHealth";

// TCP port the agent answers on, at the radio's LAN address.
inline constexpr quint16 kRadioHealthPort = 48994;

// True for the agent's row in the radio's installed list (a legacy waveform).
inline bool isRadioHealthWaveform(const QString& name, bool isContainer)
{
    return !isContainer && name == QLatin1String(kRadioHealthWaveformName);
}

}  // namespace AetherSDR
