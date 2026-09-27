#pragma once

// ONE predicate for whether SpectrumWidget runs its own fixed SMOOTH_ALPHA EMA
// on the trace, because MainWindow asks it in two places (every connect and
// disconnect, and every pan created after connect) and a test has to be able
// to ask it too.
//
// RFC #5782 §8, as ruled when #5814 landed: a backend that averages the
// panadapter itself declares RadioCapabilities::backendPanAveraging, and the
// widget then skips its EMA so the operator's FFT AVG is not averaged twice.
// On ANAN that is WDSP's analyzer; on HL2 it is Hl2Spectrum's time-constant
// average. Disconnected, backendCapabilities() describes no radio, so the
// widget falls back to its own smoothing.
//
// Same shape and same directory as PanZoomModeGate.h: UI policy, no Qt type,
// no engine type, testable without linking the GUI.

namespace AetherSDR {

// `backendAveragesPan` is RadioCapabilities::backendPanAveraging.has_value()
// off RadioModel::backendCapabilities().
[[nodiscard]] constexpr bool clientFftSmoothingEnabled(
    bool connected, bool backendAveragesPan) noexcept
{
    return !(connected && backendAveragesPan);
}

}  // namespace AetherSDR
