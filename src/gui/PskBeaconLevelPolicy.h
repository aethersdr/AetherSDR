#pragma once

// The WSPR beacon's generated audio level, as pure decisions.
//
// This is the level the beacon generator is asked for, not RF power: it is the
// amplitude of the audio handed to the transmit chain, and what happens to it
// afterwards is the difference the two constants below encode.
//
// It lives in a header, evaluated by PskReporterMapDialog rather than copied
// into it, so the suite exercises the SAME expressions the dialog runs. A test
// against a re-typed copy of a mapping proves only that two copies agree.
//
// See PskReporterMapDialog's beacon-level block for WHY each answer is shaped
// this way; this header is the decision only.

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
//
// ARMED IS THE FIRST TEST AND IT IS ABSOLUTE. Once a beacon is armed the level
// it was armed with is the level that goes out; a radio status change arriving
// mid-slot must not move it (Principle VI — the operator's intent to transmit
// is the level they saw when they pressed the button).
//
// A stored level is a deliberate choice and outranks the default. It is stored
// PER RADIO, so "the operator set this" means they set it for THIS radio; a
// level chosen on a Flex is not an instruction about the HL2 that is now
// connected. That is the whole reason this takes an optional rather than
// reading one app-global key.
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

// Whether a legacy app-global level may be claimed into THIS radio's document.
//
// The old key was one value for every radio, and on a host-modulating backend
// it never described an on-air level at all: while Hl2TxDsp's ALC still had its
// makeup half it normalised anything from roughly -45 dBFS up onto
// alcTargetPeak, so the control was inert and whatever sits in that key was
// never a choice ABOUT the air. Importing it into an HL2's document would
// freeze a non-choice into the one place that now decides an unattended
// transmit level.
//
// Where the radio modulates, the same key DID reach the air through the
// radio's own mic gain, so it is a real setting and is carried across.
[[nodiscard]] constexpr bool legacyBeaconLevelAppliesTo(bool hostModulates) noexcept
{
    return !hostModulates;
}

} // namespace AetherSDR::psk
