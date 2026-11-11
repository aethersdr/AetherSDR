#pragma once

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QStringList>

namespace AetherSDR {

// Additional receive-client semantics are opt-in; confirmed authority alone
// does not change an existing backend's CAT or TCI dialect.
inline bool usesReceiveClientFeature(const RadioModel* model, ReceiveClientControl::Feature feature)
{
    if (!model) { return false; }
    const auto control = model->backendCapabilities().receiveClientControl;
    return control && control->supports(feature);
}

inline bool usesDeclaredReceiveModes(const RadioModel* model, const SliceModel* slice)
{
    return slice && usesReceiveClientFeature(model, ReceiveClientControl::Feature::NativeModes);
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
