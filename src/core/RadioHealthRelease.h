#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// The radio health agent ("Install radio health tools" in the Waveforms
// window): a legacy waveform that reports the radio's free storage, memory
// and CPU on the LAN. AetherSDR downloads exactly this file and refuses any
// other bytes. `tools/radio-health/build.py` at the release tag in kUrl
// rebuilds the identical file with zig 0.17.
//
// Bump all four together when a new agent release is published.
struct RadioHealthRelease {
    static constexpr const char* kVersion = "0.1.0";
    static constexpr const char* kUrl =
        "https://github.com/aethersdr/AetherSDR/releases/download/"
        "aether-health-v0.1.0/AetherHealth-0.1.0.ssdr_waveform";
    static constexpr const char* kSha256 =
        "68792b711f09a0a4d5dbf6b614da3bcb588c23c9a9972a8b59473906988a84bc";
    static constexpr qint64 kSize = 63098;
};

// The agent's waveform name (tools/radio-health/package/AetherHealth.cfg).
// The Waveforms list hides it: the checkbox is its only control.
inline constexpr const char* kRadioHealthWaveformName = "AetherHealth";

// True for the agent's row in the radio's installed list (a legacy waveform).
inline bool isRadioHealthWaveform(const QString& name, bool isContainer)
{
    return !isContainer && name == QLatin1String(kRadioHealthWaveformName);
}

// TCP port the agent answers on, at the radio's LAN address.
inline constexpr quint16 kRadioHealthPort = 48994;

}  // namespace AetherSDR
