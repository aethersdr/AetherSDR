#pragma once

#include <optional>

#include <QMetaType>

namespace AetherSDR {

// Normalized manual-notch delta: a Flex TNF, or a host-DSP null on a radio with no
// DSP of its own. Present-only like SliceDelta, so a centre+width change can be
// one edit (one mask rebuild on host DSP); no caller builds that combined delta
// yet. `active` is not round-tripped (TnfEntry has no such field). `depth` and
// `permanent` are Flex-only; a backend without them ignores them, and the UI
// hides them per RadioCapabilities::notchHasDepth.
struct NotchDelta {
    // ABSOLUTE RF Hz, not an audio-frequency offset. A notch is placed on the
    // interferer and stays on it while the operator tunes.
    std::optional<double> centerHz;
    std::optional<double> widthHz;
    // Per-notch bypass, independent of the global enable.
    std::optional<bool>   active;
    // Flex only: 1 normal, 2 deep, 3 very deep.
    std::optional<int>    depthDb;
    // Flex only: the radio keeps this notch across a power cycle.
    std::optional<bool>   permanent;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::NotchDelta)
