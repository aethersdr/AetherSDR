#pragma once

// WHERE THE HEADPHONE CONTROLS GO when the radio has no headphone mixer we can
// reach.
//
// The title bar's headphone mute and volume, the MIDI "Headphone Volume" and
// "Master Volume" sliders, and the controller wheel action
// WheelHeadphoneVolume all drive the RADIO's own lineout/headphone mixer with
// `mixer headphone|lineout ...` -- FlexLib wire text. RadioModel::sendCmd drops
// that at hasCommandPlane() on every other family, so on a Hermes-Lite 2 the
// slider moved, the glyph muted, and the audio in the operator's ears did not
// change at all.
//
// On such a radio the audio the operator hears IS this computer's output: the
// HL2 demodulates on the host and plays through AudioEngine's RX sink. The only
// output level and the only mute that exist are AudioEngine::setRxVolume() and
// AudioEngine::setMuted() -- the same two the title bar's master slider and
// speaker mute already drive through MainWindow::applyMasterVolume() when PC
// Audio is on. So the headphone controls FOLLOW that output instead of dying:
// they become a second handle on the one output there is, and the two sliders
// (and the two mutes) track each other.
//
// ON A FLEX NOTHING CHANGES. A Flex has a real headphone jack with its own
// mixer channel, the controls keep their wire text, and this returns false the
// moment a command plane exists.
//
// "RX audio plays on this computer" is PC Audio being on. A radio with no
// command plane and PC Audio off (an Icom playing through its own speaker) has
// no output here to follow, so the controls keep their old path and its loud
// drop -- mapping them onto a sink nobody is listening to would be a second
// dead control, not a fix.
//
// Pure, no Qt and no engine type, so the test includes it directly; the
// PanZoomModeGate.h shape.

namespace AetherSDR {

[[nodiscard]] constexpr bool headphoneFollowsLocalOutput(
    bool connected, bool hasCommandPlane, bool pcAudioEnabled) noexcept
{
    return connected && !hasCommandPlane && pcAudioEnabled;
}

// WHAT THE HEADPHONE SLIDER AND GLYPH SHOULD SHOW. Three answers, not two:
// with no radio connected there is no output and no mixer for them to report,
// so they are left as they are. capabilitiesChanged fires on the disconnect
// edge too, and reading the radio mixer there would snap the slider to
// RadioModel's default gain -- a number no radio ever reported -- while the
// output it had been following stayed where it was.
enum class HeadphoneLevelSource { LocalOutput, RadioMixer, Unchanged };

[[nodiscard]] constexpr HeadphoneLevelSource headphoneLevelSource(
    bool connected, bool hasCommandPlane, bool pcAudioEnabled) noexcept
{
    if (headphoneFollowsLocalOutput(connected, hasCommandPlane, pcAudioEnabled))
        return HeadphoneLevelSource::LocalOutput;
    return connected ? HeadphoneLevelSource::RadioMixer
                     : HeadphoneLevelSource::Unchanged;
}

}  // namespace AetherSDR
