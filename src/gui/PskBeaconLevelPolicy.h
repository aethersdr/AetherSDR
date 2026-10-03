#pragma once

// WSPR beacon audio level (generator amplitude, not RF power) as pure
// decisions, evaluated by PskReporterMapDialog so tests run the same
// expressions. The reasoning lives in the dialog's beacon-level block.

#include <optional>

namespace AetherSDR::psk {

// A host-modulating backend generates near the top of the range, WSJT-X style,
// and the operator attenuates from there: our own ALC is the last thing the
// audio passes, so the level asked for here is the level transmitted.
inline constexpr int kBeaconLevelHostModulatedDbFs = -3;

// A radio that modulates on ITS side (Flex, Icom) applies its own mic gain and
// ALC to this audio, against an input the operator has already levelled.
// Raising the source 17 dB would overdrive it.
inline constexpr int kBeaconLevelRadioModulatedDbFs = -20;

// The default level for a backend, by capability — never by family name.
//
// The question is "does OUR ALC see this audio", which is
// TransmitModel::hostModulation() (hostModulates && canTransmit), NOT
// AudioEngine::hostModulation() (takesTxAudioOverSeam && canTransmit). The two
// come apart on exactly one backend: IcomCivBackend sets
// takesTxAudioOverSeam=true with hostModulates=false, because the RADIO
// modulates while the host still SHIPS the audio. Reading the engine's flag
// here would hand an Icom -3 dBFS, which is the precise harm this avoids.
[[nodiscard]] constexpr int beaconLevelDefaultDbFs(bool hostModulates) noexcept
{
    return hostModulates ? kBeaconLevelHostModulatedDbFs
                         : kBeaconLevelRadioModulatedDbFs;
}

// What the level control should read, or nothing when it must be left alone.
// Armed comes first and is absolute: the armed level is what transmits, and no
// mid-slot status may move it. A stored level outranks the default and is
// stored per radio, hence the optional.
[[nodiscard]] constexpr std::optional<int> beaconLevelToApplyDbFs(
    bool beaconArmed, std::optional<int> storedDbFs, bool hostModulates) noexcept
{
    if (beaconArmed) {
        return std::nullopt;
    }
    if (storedDbFs.has_value()) {
        return storedDbFs;
    }
    return beaconLevelDefaultDbFs(hostModulates);
}

// Whether the legacy app-global level may be claimed into this radio's
// document. On a host-modulating backend it never set an on-air level (the old
// ALC makeup normalised it to alcTargetPeak), so it is not imported; where the
// radio modulates it reached the air via mic gain and is carried across.
[[nodiscard]] constexpr bool legacyBeaconLevelAppliesTo(bool hostModulates) noexcept
{
    return !hostModulates;
}

} // namespace AetherSDR::psk
