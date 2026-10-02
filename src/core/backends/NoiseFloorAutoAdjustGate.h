#pragma once

// The one gate deciding whether noise-floor auto-adjust may move the display
// reference level, shared by SpectrumWidget and its test. Either property ends
// the loop, so it is an OR:
//   radioOwnsDbmScale  the radio accepts a display dBm range and ECHOES it back
//                      (Flex yes; HL2, ANAN, RTL-SDR, Icom no).
//   panBinsAbsolute    the bins do not move with the reference level, so the loop
//                      converges in one step (RadioCapabilities::panBinsAbsolute();
//                      an undeclared backend is not assumed absolute).
// With NEITHER, the loop chases a retreating floor (measured 24 dB/s on an
// IC-9700). Qt-free so both sides of the widget's setters can use it.

namespace AetherSDR {

// True when the auto-floor may run. The early return in
// SpectrumWidget::applyNoiseFloorAutoAdjust fires when this is false — that is,
// only when NEITHER property holds.
constexpr bool noiseFloorAutoAdjustAllowed(bool radioOwnsDbmScale,
                                           bool panBinsAbsolute) noexcept
{
    return radioOwnsDbmScale || panBinsAbsolute;
}

}  // namespace AetherSDR
