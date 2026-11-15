#pragma once

#include "core/backends/flex/FlexTransmitServiceStatus.h"
#include "models/DvkModel.h"
#include "models/NavtexModel.h"

namespace AetherSDR {

// A radio "dvk …" / "navtex …" status, decoded as the status ingress decodes it.
inline void applyDvkStatus(DvkModel& model, const QString& object, const QMap<QString, QString>& fields)
{
    if (const auto observation = decodeFlexDvkStatus(object, fields)) { model.applyObservation(*observation); }
}
inline void applyNavtexStatus(NavtexModel& model, const QString& object, const QMap<QString, QString>& fields)
{
    if (const auto observation = decodeFlexNavtexStatus(object, fields)) { model.applyObservation(*observation); }
}

} // namespace AetherSDR
