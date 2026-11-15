#pragma once

#include "models/RadioModel.h"

#include <functional>

class QObject;

namespace AetherSDR {

class SliceModel;

using MemoryCreateCallback =
    std::function<void(int code, const QString& body, int memoryIndex)>;

MemoryEntry captureMemoryFromSlice(const RadioModel& model,
                                   const SliceModel& slice,
                                   const QString& name = QString());
void createMemoryFromSlice(RadioModel* model,
                           const SliceModel* slice,
                           const QString& name = QString(),
                           QObject* callbackContext = nullptr,
                           MemoryCreateCallback cb = {});

} // namespace AetherSDR
