#pragma once

#include "core/backends/hl2/Hl2FilterBoard.h"

namespace AetherSDR {

class RadioSettingsScope;

// Owned configuration for the "Filter Board" settings page (ticket #12).
// Radio-scoped state — the manual table describes one physical J16 board's
// wiring — so it lives in the radio-scoped feature-document store (RFC
// #4603 proposal A, Constitution Principle V; AGENTS.md "Radio-Scoped
// Feature Documents") behind a caller-supplied RadioSettingsScope, the same
// mechanism Hl2FreqCal uses, rather than a flat AppSettings key.
class Hl2FilterBoardSettings {
public:
    static constexpr const char* kFeature = "FilterBoard";
    static constexpr int kSchemaVersion = 1;

    // Whether the operator has opted into hand-configuring the filter board.
    // False is the load-bearing default: automatic band switching
    // (Hl2Backend::applyBandFilter) stays exactly as it is today unless this
    // is explicitly turned on.
    static bool manualEnabled(const RadioSettingsScope& scope);
    static void setManualEnabled(const RadioSettingsScope& scope, bool enabled);

    // The manual per-band table. Absent/corrupt storage returns an EMPTY
    // table rather than a guess — manualFilterByte() already treats an
    // absent band as kOcNone (Hl2FilterBoard.h), which is the same fail-safe
    // "release every relay" behavior an empty table produces here.
    static hl2::ManualFilterTable table(const RadioSettingsScope& scope);
    static void setTable(const RadioSettingsScope& scope, const hl2::ManualFilterTable& table);
};

}  // namespace AetherSDR
