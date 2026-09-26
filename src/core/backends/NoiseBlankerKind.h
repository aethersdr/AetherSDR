#pragma once

#include <QMetaType>

// WHICH impulse blanker a slice is asking for, and what fills the hole it
// leaves. Its own header, beside SliceDelta.h, because it is shared vocabulary:
// IRadioBackend declares the verb that carries it, SliceModel holds the
// operator's choice, and the automation server reports it back. Putting it in
// IRadioBackend.h would drag the whole seam into SliceModel's includes for two
// enums.
//
// Nothing here knows about WDSP. A backend whose blanker is the radio's own
// firmware maps Advanced onto whatever single blanker it has (see the verb's
// comment in IRadioBackend.h); a host-side backend maps these onto its DSP's
// own enums. The two vocabularies are deliberately separate so that adding a
// third algorithm here is not a change to the engine, and vice versa.
namespace AetherSDR {

enum class NoiseBlankerKind : int {
    // No blanker. The operator's level and fill choice are REMEMBERED across
    // Off, because the control is a cycle: Off -> NB -> NB2 -> Off, and losing
    // the level on every pass through Off would make the slider un-settable.
    Off = 0,
    // WDSP's ANB (nob.c): detects an impulse and gates the window to zero.
    // What #5824 shipped, and what every reference client calls NB.
    Impulse = 1,
    // WDSP's NOB (nobII.c): detects the same impulse and RECONSTRUCTS the
    // window instead of zeroing it, which is why it is worth having both — it
    // is better on impulses inside a wanted signal and worse on dense noise,
    // so no client replaces one with the other.
    Advanced = 2,
};

// What the Advanced blanker puts in the blanked window. WDSP's own numbering
// (nobII.c, the switch on a->mode), kept as its values so the mapping to
// SetEXTNOBMode is an identity rather than a table that can rot.
//
// Meaningless for Impulse, which always zeroes. Held anyway while Impulse runs,
// for the same reason the level is held across Off.
enum class NoiseBlankerFill : int {
    Zero = 0,        // the window goes to zero, as the Impulse blanker does
    SampleHold = 1,  // hold the filtered sample from BEFORE the impulse
    MeanHold = 2,    // the mean of the samples before and after it
    HoldSample = 3,  // hold the filtered sample from AFTER the impulse
    Interpolate = 4, // straight line from the one before to the one after
};

// The default is WDSP's own and every reference client's: zero-fill. A
// reconstruction is a guess, and the operator should have to ask for it.
constexpr NoiseBlankerFill kDefaultNoiseBlankerFill = NoiseBlankerFill::Zero;

[[nodiscard]] constexpr bool isValidNoiseBlankerFill(int value) noexcept
{
    return value >= static_cast<int>(NoiseBlankerFill::Zero)
        && value <= static_cast<int>(NoiseBlankerFill::Interpolate);
}

[[nodiscard]] constexpr bool isValidNoiseBlankerKind(int value) noexcept
{
    return value >= static_cast<int>(NoiseBlankerKind::Off)
        && value <= static_cast<int>(NoiseBlankerKind::Advanced);
}

} // namespace AetherSDR

// Both cross a thread boundary as queued Q_ARGs (Hl2Backend marshals control
// verbs onto its I/O thread, AnanBackend onto the DSP object's). An unregistered
// type there does not fail loudly -- invokeMethod warns and DROPS the call,
// which would silently break the control.
Q_DECLARE_METATYPE(AetherSDR::NoiseBlankerKind)
Q_DECLARE_METATYPE(AetherSDR::NoiseBlankerFill)
