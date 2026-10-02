#pragma once

// WHICH OF THE RADIO-MIXER CONTROLS EXIST on the connected radio.
//
// The title bar's headphone mute and volume, the MIDI "Headphone Volume" and
// "Master Volume" sliders, and the controller wheel action
// WheelHeadphoneVolume all drive the RADIO's own lineout/headphone mixer with
// `mixer headphone|lineout ...` -- FlexLib wire text. RadioModel::sendCmd drops
// that at hasCommandPlane() on every other family, so on a Hermes-Lite 2 the
// headphone slider moved, the glyph muted, and nothing happened.
//
// HEADPHONE PAIR: such a radio has no headphone output, so the pair is
// UNAVAILABLE there -- dimmed, never hidden, the reason on its tooltip and
// accessibleDescription (theme-style-guide.md, Three-state controls) -- and the
// MIDI knob and wheel action that drive the same mixer channel refuse through
// the one-shot notice. With no radio connected there is nothing to be honest
// ABOUT, so the pair is available, as every capability gate here restores on
// disconnect. A Flex keeps the pair live and its wire text: available the
// moment a command plane exists.
//
// MASTER KNOB: the title bar's master slider already drives this computer's
// output through MainWindow::applyMasterVolume() when PC Audio is on, and on a
// radio with no command plane that output is what the operator hears. The MIDI
// Master Volume knob takes that same path there; with PC Audio off there is no
// output here and no mixer on the radio, and it refuses.
//
// Pure, no Qt and no engine type, so the test includes it directly; the
// PanZoomModeGate.h shape.

namespace AetherSDR {

[[nodiscard]] constexpr bool headphoneControlsAvailable(
    bool connected, bool hasCommandPlane) noexcept
{
    return !connected || hasCommandPlane;
}

[[nodiscard]] constexpr bool masterKnobDrivesLocalOutput(
    bool connected, bool hasCommandPlane, bool pcAudioEnabled) noexcept
{
    return connected && !hasCommandPlane && pcAudioEnabled;
}

}  // namespace AetherSDR
