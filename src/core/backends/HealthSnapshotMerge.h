#pragma once

// Merge two health snapshots; family-neutral so AutomationServer::doHealth needs
// no family header. A key the winner declares but leaves OUT of `values` means
// "not reported" and must not erase the base's value. hl2MergeHealth in
// Hl2TelemetrySource.h forwards here so the HL2 tests pin this function.

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
