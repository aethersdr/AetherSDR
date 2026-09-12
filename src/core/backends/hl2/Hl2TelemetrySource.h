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

#include <QString>

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
// ONE RULE IS LOAD-BEARING AND EASY TO GET BACKWARDS: a key the winner declares
// but leaves OUT of `values` means "not reported", and must not erase a value
// the base does have. Overwriting a real reading with nothing is how a working
// number becomes a dash — and this snapshot spells "never reported" as an
// absent value precisely so that the difference survives to the UI.
[[nodiscard]] inline IRadioBackend::HealthSnapshot
hl2MergeHealth(IRadioBackend::HealthSnapshot base,
               const IRadioBackend::HealthSnapshot& winner)
{
    for (const QString& key : winner.order) {
        if (!base.labels.contains(key))
            base.order.push_back(key);
        if (const auto l = winner.labels.constFind(key); l != winner.labels.constEnd())
            base.labels.insert(key, *l);
        if (const auto s = winner.sections.constFind(key); s != winner.sections.constEnd())
            base.sections.insert(key, *s);
        if (const auto v = winner.values.constFind(key); v != winner.values.constEnd())
            base.values.insert(key, *v);
    }
    return base;
}

}  // namespace AetherSDR::hl2
