#pragma once

#include <cmath>
#include <cstddef>

// The ANAN receiver's own audio stage: mute, AF gain and left/right balance,
// applied to demodulated stereo before it leaves the backend.
//
// WHY IT IS HERE AND NOT IN THE ENGINE. docs/architecture/audio-pipeline.md
// states the contract the other end relies on: "Radio speaker audio enters as
// stereo with the radio's per-slice pan already applied", and the only
// client-side pan stage runs for virtual KiwiSDR profiles alone. AudioEngine's
// setRxVolume()/setMuted() are the MASTER controls -- one sink volume for
// everything the operator is listening to -- so a per-receiver control cannot
// live there without the two meanings colliding. IRadioBackend says the same
// thing from the other direction: a backend that demodulates on this host has to
// apply these itself, and until it does, the operator's mute moves the fader
// while the audio keeps playing. That was ANAN's behaviour.
//
// Socket-free, Qt-free and DSP-free on purpose, so the law below is checked by
// arithmetic rather than by listening.
namespace AetherSDR::anan {

// Silence, and the gain range in dB either side of it.
//
// A dB law, not a linear one, because a fader is a perceptual control: ear
// response to amplitude is roughly logarithmic, so a linear 0..100 -> 0.0..1.0
// scale spends most of its travel in changes that are barely audible and then
// collapses the useful range into the bottom few percent. 40 dB is the span that
// leaves usable resolution at every position while still reaching "audibly off"
// at the bottom.
inline constexpr double kSliceAudioRangeDb = 40.0;

// 0 -> silence, 100 -> unity, and 50 -> -20 dB by construction.
//
// EXACTLY ZERO AT ZERO, not the -40 dB the curve would otherwise give. A fader
// at its bottom stop has to be silent: 10^-2 is 1% amplitude, which is quiet but
// plainly audible on a strong signal, and a control the operator has set to
// nothing that still makes noise reads as a broken control.
[[nodiscard]] inline float sliceAudioAmplitude(int gainPercent) noexcept
{
    if (gainPercent <= 0) {
        return 0.0f;
    }
    if (gainPercent >= 100) {
        return 1.0f;
    }
    const double db = -kSliceAudioRangeDb + (kSliceAudioRangeDb / 100.0) * gainPercent;
    return static_cast<float>(std::pow(10.0, 0.05 * db));
}

// Balance, on the 0 left / 50 centre / 100 right scale the slice model already
// uses. ATTENUATES THE OPPOSITE CHANNEL AND NEVER BOOSTS EITHER: panning must not
// be able to clip a signal that was within range before it, and the client's own
// pan stage uses this same law, so a panned ANAN slice and a panned virtual
// receiver do not end up at different loudnesses at the same setting.
[[nodiscard]] inline float sliceAudioLeftPanGain(int panPercent) noexcept
{
    return panPercent >= 50 ? static_cast<float>(100 - panPercent) / 50.0f : 1.0f;
}

[[nodiscard]] inline float sliceAudioRightPanGain(int panPercent) noexcept
{
    return panPercent <= 50 ? static_cast<float>(panPercent) / 50.0f : 1.0f;
}

// Apply all three to interleaved L,R float32 in place.
//
// MUTE WINS OUTRIGHT and is not folded into the gain, so that unmuting restores
// the level the operator had set rather than whatever the gain happened to be
// left at. Gain before balance: balance only ever attenuates, so the order is
// not observable in the output, but it is observable in intent -- gain is "how
// loud is this receiver", balance is "where is it", and reading them in that
// order is what makes a muted slice with a live gain setting coherent.
inline void applySliceAudioInPlace(float* stereo, std::size_t frames, bool muted,
                                  int gainPercent, int panPercent) noexcept
{
    if (stereo == nullptr || frames == 0) {
        return;
    }
    if (muted) {
        for (std::size_t i = 0; i < frames * 2; ++i) {
            stereo[i] = 0.0f;
        }
        return;
    }
    const float amplitude = sliceAudioAmplitude(gainPercent);
    const float left = amplitude * sliceAudioLeftPanGain(panPercent);
    const float right = amplitude * sliceAudioRightPanGain(panPercent);
    // Unity on both sides is the overwhelmingly common case (gain 100, centred),
    // and this runs on every audio block, so skip the multiply rather than
    // multiply by 1.0f forty-eight thousand times a second.
    if (left == 1.0f && right == 1.0f) {
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        stereo[i * 2]     *= left;
        stereo[i * 2 + 1] *= right;
    }
}

}  // namespace AetherSDR::anan
