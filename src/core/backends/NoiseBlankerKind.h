#pragma once

#include <QMetaType>

// Which impulse blanker a slice asks for, and what fills the window it blanks.
// Shared by the seam, SliceModel and the automation server, so it has its own
// header; nothing here knows about WDSP (see WdspNoiseBlanker.h).
namespace AetherSDR {

enum class NoiseBlankerKind : int {
    // No blanker. Level and fill are kept across Off, which the cycle passes.
    Off = 0,
    // WDSP's ANB (nob.c): gates the blanked window to zero. "NB".
    Impulse = 1,
    // WDSP's NOB (nobII.c): reconstructs the window instead. "NB2"; better on
    // impulses inside a wanted signal, worse on dense noise.
    Advanced = 2,
};

// What Advanced puts in the blanked window, in WDSP's own numbering (nobII.c)
// so SetEXTNOBMode takes it as is. Ignored by Impulse, but kept while it runs.
enum class NoiseBlankerFill : int {
    Zero = 0,        // the window goes to zero, as the Impulse blanker does
    SampleHold = 1,  // hold the filtered sample from BEFORE the impulse
    MeanHold = 2,    // the mean of the samples before and after it
    HoldSample = 3,  // hold the filtered sample from AFTER the impulse
    Interpolate = 4, // straight line from the one before to the one after
};

// WDSP's default: zero-fill.
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

// Both cross threads as queued Q_ARGs; an unregistered type makes
// invokeMethod drop the call with only a warning.
Q_DECLARE_METATYPE(AetherSDR::NoiseBlankerKind)
Q_DECLARE_METATYPE(AetherSDR::NoiseBlankerFill)
