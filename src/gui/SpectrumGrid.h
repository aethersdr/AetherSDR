#pragma once

// dB grid step, shared by the main panadapter and the mini-pan.
//
// Same reasoning as gui/FftHeatMap.h: the mini-pan is a magnifier on the main
// trace, so its grid has to mean the same thing. A second copy of this ladder
// would put the two views' rules at different dB values and quietly turn the
// mini-pan's grid back into decoration.

namespace AetherSDR::SpectrumGrid {

// Pick a round dB step for roughly five horizontal rules across `dynamicRange`,
// matching the steps the dBm scale strip labels.
inline float dbStep(float dynamicRange)
{
    const float raw = dynamicRange / 5.0f;
    if (raw >= 20.0f) return 20.0f;
    if (raw >= 10.0f) return 10.0f;
    if (raw >= 5.0f)  return 5.0f;
    return 2.0f;
}

} // namespace AetherSDR::SpectrumGrid
