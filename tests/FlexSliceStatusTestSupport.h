#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexSliceStatus.h"
#include "models/RadioModel.h"

namespace AetherSDR {

inline SliceObservation makeFlexSliceObservation(int id, QMap<QString, QString> fields,
                                                  bool removed = false)
{
    if (removed) { fields.insert(QStringLiteral("in_use"), QStringLiteral("0")); }
    auto observation = FlexSliceStatus::decode(QStringLiteral("slice %1").arg(id), fields);
    if (!observation) { return {}; }
    observation->transportEpoch = std::make_shared<std::atomic_bool>(true);
    return *observation;
}

inline void applyFlexSliceStatus(RadioModel& model, int id,
                                const QMap<QString, QString>& fields, bool removed = false)
{
    model.applySliceObservationForTest(makeFlexSliceObservation(id, fields, removed));
}

inline void publishFlexSliceStatus(FlexBackend& backend, int id,
                                  const QMap<QString, QString>& fields)
{
    backend.acceptSliceObservation(makeFlexSliceObservation(id, fields));
}

} // namespace AetherSDR
