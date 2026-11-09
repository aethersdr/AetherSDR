#pragma once

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QStringList>

namespace AetherSDR {

// Confirmed profiles must declare their native vocabulary; legacy optimistic CAT
// aliases retain their established dialect. A disconnected list is no authority.
inline bool usesDeclaredReceiveModes(const RadioModel* model, const SliceModel* slice)
{
    return model && slice && slice->receiveControlsConfirmed();
}

inline QStringList declaredReceiveModes(const RadioModel* model, const SliceModel* slice, bool writable)
{
    if (!usesDeclaredReceiveModes(model, slice) || !model->isConnected()
        || model->slice(slice->sliceId()) != slice) { return {}; }
    const auto control = model->backendCapabilities().receiveModeControl;
    if (!control) { return {}; }
    if (writable || slice->modeList().isEmpty()) { return control->modes; }
    return slice->modeList();
}

} // namespace AetherSDR
