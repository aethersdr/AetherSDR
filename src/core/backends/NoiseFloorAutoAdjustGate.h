#pragma once

// The ONE gate that decides whether the noise-floor auto-adjust may move the
// display reference level, kept in a header of its own so the widget and its
// test read the same predicate rather than two copies that can drift.
//
// It takes two INDEPENDENT properties, because the single flag it replaces was
// being asked two different questions and on a raw-IQ radio the answers differ:
//
//   radioOwnsDbmScale  — the radio accepts a display dBm range and ECHOES it
//                        back. Flex: yes. HL2, ANAN, RTL-SDR, Icom: no.
//   panBinsAbsolute    — the spectrum bins do not move when the reference level
//                        moves, so the loop's own measurement is stable under
//                        its own correction and it converges in one step.
//
// Either one alone terminates the loop, so the gate is an OR. An echo ends it
// by confirmation; absolute bins end it by giving it a fixed target. With
// NEITHER, the loop measures a floor that retreats as fast as it corrects and
// ratchets — measured at a linear 24 dB/s on an IC-9700, walking the scale to
// -1882 dBm. See RadioCapabilities::radioOwnsDbmScale and
// PanAmplitudeModel::binsAbsolute for both derivations.
//
// Deliberately free of Qt and of RadioCapabilities itself: SpectrumWidget is
// handed the two values through setters (setRadioOwnsDbmScale /
// setPanBinsAbsolute) rather than reaching for capabilities from inside the
// widget, and this header has to be usable from both sides of that seam.
//
// The second argument comes from RadioCapabilities::panBinsAbsolute(), the
// accessor over std::optional<PanAmplitudeModel> — an UNDECLARED backend is
// not assumed to have absolute bins, and the OR's first term keeps the gate
// permissive for it anyway.

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
