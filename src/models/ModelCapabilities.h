#pragma once

#include <QString>

namespace AetherSDR {

// Per-model feature flags mirroring FlexLib/ModelInfo.cs (Has2Meters,
// Has4Meters, HasLoopA, HasLoopB, ...).  Authority for any UI surface
// that varies by model capability — the band selector in the Spectrum
// Overlay Band menu is the first consumer (#695), additional flags
// should be added here as more `model.contains("6700")`-style checks
// get migrated.
struct ModelCapabilities {
    bool has4Meters{false};  // Built-in 70 MHz transverter (FLEX-6500 Region 1, FLEX-6700)
    bool has2Meters{false};  // Built-in 144 MHz transverter (FLEX-6700)
    bool hasLoopA{false};    // RX loop/preselector path (FLEX-6500, FLEX-6700)
    bool hasLoopB{false};    // Second RX loop path (FLEX-6700)
};

// Returns capabilities for the given model string.  Uses substring match
// (case-insensitive) so vendor suffixes like "FLEX-6700/A" still resolve
// to the 6700 entry.  Unknown models default to all-false — forward-
// compatible for radios released after this build.
ModelCapabilities capabilitiesFor(const QString& model);

} // namespace AetherSDR
