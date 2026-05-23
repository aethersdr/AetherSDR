#pragma once

#include "models/RadioModel.h"

#include <functional>

class QObject;

namespace MasterSDR {

class SliceModel;

struct MemoryUpdateData {
    QString commandSuffix;
    QMap<QString, QString> kvs;
};

using MemoryCreateCallback =
    std::function<void(int code, const QString& body, int memoryIndex)>;

QString encodeMemoryText(const QString& value);
MemoryEntry captureMemoryFromSlice(const RadioModel& model,
                                   const SliceModel& slice,
                                   const QString& name = QString());
MemoryUpdateData buildMemoryUpdateData(const MemoryEntry& memory);
void createMemoryFromSlice(RadioModel* model,
                           const SliceModel* slice,
                           const QString& name = QString(),
                           QObject* callbackContext = nullptr,
                           MemoryCreateCallback cb = {});

} // namespace MasterSDR
