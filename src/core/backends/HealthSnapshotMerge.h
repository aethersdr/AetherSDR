#pragma once

// Merge two health snapshots. Family-neutral, because two consumers now need it
// and one of them (`AutomationServer::doHealth`) must not include a family
// header to get it.
//
// ONE RULE IS LOAD-BEARING AND EASY TO GET BACKWARDS: a key the winner declares
// but leaves OUT of `values` means "not reported", and must not erase a value
// the base does have. Overwriting a real reading with nothing is how a working
// number becomes a dash — and this snapshot spells "never reported" as an
// absent value precisely so that the difference survives to the UI.
//
// Moved here from `backends/hl2/Hl2TelemetrySource.h`, which keeps
// `hl2MergeHealth` as a forwarder so the HL2 tests that pin this rule against
// the HL2 call site continue to pin the same function rather than a copy.

#include "core/backends/IRadioBackend.h"   // HealthSnapshot

#include <QString>

namespace AetherSDR {

[[nodiscard]] inline IRadioBackend::HealthSnapshot
mergeHealthSnapshots(IRadioBackend::HealthSnapshot base,
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

}  // namespace AetherSDR
