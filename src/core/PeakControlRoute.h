#pragma once

// Where Peak control actually runs (RFC #4836): in AetherTX's chain, or in the
// radio's modulator when the radio offers it (the HL2's WDSP TXA channel) and
// the operator chose it. Never both: two clippers in series only add
// distortion. Pure, so the table is testable without a radio.

#include "core/ClientCessb.h"

namespace AetherSDR::PeakControlRoute {

struct Resolved {
    bool chainActive = false;
    bool modulatorActive = false;
    // The operator chose the modulator but this radio has none: the chain runs
    // it, and the page says so.
    bool fellBackToChain = false;
};

[[nodiscard]] constexpr Resolved resolve(bool enabled, ClientCessb::Route route,
                                         bool modulatorAvailable) noexcept
{
    const bool wantsModulator = route == ClientCessb::Route::Modulator;
    const bool useModulator = wantsModulator && modulatorAvailable;
    Resolved r;
    r.chainActive = enabled && !useModulator;
    r.modulatorActive = enabled && useModulator;
    r.fellBackToChain = wantsModulator && !modulatorAvailable;
    return r;
}

}  // namespace AetherSDR::PeakControlRoute
