#pragma once

#include "core/backends/flex/FlexDisplayStatus.h"
#include "models/PanadapterModel.h"

namespace AetherSDR {

inline void applyPanReport(PanadapterModel& pan, const QMap<QString, QString>& fields)
{
    const auto observation = decodeFlexDisplayStatus(QStringLiteral("display pan %1").arg(pan.panId()), fields);
    if (!observation) { return; }
    if (observation->ownerHandle) { pan.setOwnerHandle(*observation->ownerHandle); }
    if (observation->pairedId) { pan.setWaterfallId(*observation->pairedId); }
    pan.applyDisplayValues(observation->values, {});
}

} // namespace AetherSDR
