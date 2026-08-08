#pragma once

#include <optional>

#include <QMetaType>

namespace AetherSDR {

// Normalized, vendor-neutral manual-notch delta — a Flex TNF, or the host-DSP
// null that stands in for one on a radio with no DSP of its own.
//
// Same contract as SliceDelta: a populated optional means "the caller means
// this", an empty one means "leave it alone". That is what lets one call carry
// a single menu toggle and also PERMITS a centre+width change to arrive as one
// edit rather than two — worth having on a host-DSP backend, where each edit
// rebuilds the whole filter mask. No caller builds that combined delta yet:
// SpectrumWidget's drag emits tnfMoveRequested and tnfWidthRequested as
// separate signals, and the bridge's `notch set` applies one key at a time, so
// today a diagonal drag still costs two rebuilds. The seam allows the fix; it
// is not itself the fix.
//
// `active` is likewise accepted but not round-tripped: TnfEntry has no such
// field, so TnfModel::applyNotchDelta drops it. It is backend-internal — a
// per-notch bypass a backend may honour and the model cannot currently show.
//
// It is also how the seam stays honest about features only some radios have.
// `depth` is a Flex concept — three notch depths — and a WDSP notched bandpass
// is a full null with no depth parameter at all; `permanent` means "survives a
// power cycle", which requires somewhere in the radio to survive into. A
// backend that has neither ignores those fields, and the UI never offers them
// in the first place because RadioCapabilities::notchHasDepth said so. The
// alternative — a positional setNotch(id, centre, width, active) — silently
// dropped both on the way through, which is a worse kind of quiet.
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
