#pragma once

// Whether SpectrumWidget runs its own fixed SMOOTH_ALPHA EMA on the trace. One
// predicate because MainWindow asks it in two places (connect/disconnect, and
// every pan created after connect) and a test asks it too. A backend that
// averages the panadapter itself declares RadioCapabilities::backendPanAveraging
// and the widget then skips its EMA (RFC #5782 §8); disconnected, it smooths.

namespace AetherSDR {

// `backendAveragesPan` is RadioCapabilities::backendPanAveraging.has_value()
// off RadioModel::backendCapabilities().
[[nodiscard]] constexpr bool clientFftSmoothingEnabled(
    bool connected, bool backendAveragesPan) noexcept
{
    return !(connected && backendAveragesPan);
}

}  // namespace AetherSDR
