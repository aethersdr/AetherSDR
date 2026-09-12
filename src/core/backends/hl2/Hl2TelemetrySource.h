#pragma once

// Which path produced the telemetry on screen, and how two snapshots merge.
//
// THE ROW EXISTS TO TELL TWO STATES APART, and for a while it could only say
// one of them. When Hl2TelemetryService took over the four telemetry rows,
// Hl2Backend stopped publishing `telemetrySource`, so the service's value always
// won the merge and `in-band` became unreachable: during a live streaming
// session the row read `port-1025`. An attribution row that cannot attribute is
// worse than no row, because a reader takes it at face value.
//
// Worse, the service's comment claimed "the backend's own row overrides this at
// the merge point" — describing a mechanism that had been removed. A comment
// asserting a design that is not implemented reads as a reason not to check.
//
// So both halves live here as pure functions, testable without a radio, and
// both call sites use these rather than re-deciding: the same rule
// Hl2TxLevelPolicy.h states for its arithmetic — a test against a re-typed copy
// of a mapping proves only that two copies agree, and the convention error it
// is meant to catch would sit in both.

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

// In-band wins whenever it has something: it arrives at 10 Hz against the
// poller's 1–2 Hz and its cadence is ours. Stream-free answers when in-band has
// nothing, which is the disconnected, stalled and held-by-another-client cases
// the poller exists for.
//
// `connected` is required for in-band and not merely correlated with it: the
// EP6 readings persist in Hl2Telemetry after a session ends, so a disconnected
// app holding stale in-band values must NOT claim `in-band` — that is the
// frozen-reading failure this feature was built to expose, and reporting it as
// live in-band telemetry would be the feature lying about its own subject.
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
// MOVED, NOT COPIED. The rule now lives in `backends/HealthSnapshotMerge.h`
// because `AutomationServer::doHealth()` needs it and must not include a family
// header to get it. This forwarder stays so the HL2 call site and the HL2 tests
// keep naming the same function rather than a re-typed twin — the same reason
// `hl2TelemetrySource` above is shared rather than restated: a test against a
// copy of a rule proves only that two copies agree.
[[nodiscard]] inline IRadioBackend::HealthSnapshot
hl2MergeHealth(IRadioBackend::HealthSnapshot base,
               const IRadioBackend::HealthSnapshot& winner)
{
    return mergeHealthSnapshots(std::move(base), winner);
}

}  // namespace AetherSDR::hl2
