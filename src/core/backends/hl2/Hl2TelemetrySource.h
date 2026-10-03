#pragma once

// Which path produced the telemetry on screen, and how two snapshots merge.
// Pure functions that both Hl2Backend and Hl2TelemetryService call, so the
// `telemetrySource` attribution cannot drift between them.

#include "core/backends/IRadioBackend.h"   // HealthSnapshot
#include "core/backends/HealthSnapshotMerge.h"  // mergeHealthSnapshots — the rule itself

#include <QString>

#include <utility>

namespace AetherSDR::hl2 {

// The three answers, spelled once.
//
// `none` is a claim — we looked and nobody spoke — and deliberately not an empty
// string, which a reader could take for "this radio does not support it".
inline const QString kTelemetrySourceInBand   = QStringLiteral("in-band");
inline const QString kTelemetrySourcePort1025 = QStringLiteral("port-1025");
inline const QString kTelemetrySourceNone     = QStringLiteral("none");

// In-band (10 Hz) wins whenever it has something; stream-free answers
// otherwise. `connected` is required for in-band: EP6 readings persist in
// Hl2Telemetry after a session ends and must not be reported as live.
[[nodiscard]] inline QString hl2TelemetrySource(bool connected,
                                                bool haveInBand,
                                                bool haveStreamFree) noexcept
{
    if (connected && haveInBand)
        return kTelemetrySourceInBand;
    if (haveStreamFree)
        return kTelemetrySourcePort1025;
    return kTelemetrySourceNone;
}

// Merge two health snapshots, `winner` taking precedence on key collision.
//
// Forwards to backends/HealthSnapshotMerge.h (shared with
// AutomationServer::doHealth()), so HL2 code and tests call the one rule.
[[nodiscard]] inline IRadioBackend::HealthSnapshot
hl2MergeHealth(IRadioBackend::HealthSnapshot base,
               const IRadioBackend::HealthSnapshot& winner)
{
    return mergeHealthSnapshots(std::move(base), winner);
}

}  // namespace AetherSDR::hl2
