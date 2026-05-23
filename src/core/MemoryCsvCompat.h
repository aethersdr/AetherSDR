#pragma once

#include "models/RadioModel.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace MasterSDR {

struct MemoryCsvRecord {
    MemoryEntry memory;
    int rfPower{0};
    bool highlight{false};
    int highlightColor{0};
};

struct MemoryCsvParseResult {
    QList<MemoryCsvRecord> records;
    QStringList errors;

    bool ok() const { return errors.isEmpty(); }
};

class MemoryCsvCompat
{
public:
    static QByteArray serialize(const QList<MemoryCsvRecord>& records);
    static MemoryCsvParseResult parse(const QByteArray& bytes);
    static MemoryCsvRecord fromMemoryEntry(const MemoryEntry& memory);
};

} // namespace MasterSDR
