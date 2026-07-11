#pragma once

#include "models/MemoryEntry.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR {

struct MemoryCsvRecord {
    MemoryEntry memory;
    int rfPower{0};
    bool highlight{false};
    int highlightColor{0};
};

// Which on-disk CSV dialect a parse recognized. AetherSDR both reads and writes
// the SmartSDR memory CSV; it can additionally *read* CHIRP-next's generic CSV
// export so operators can migrate memories from other HF/VHF radios.
enum class MemoryCsvFormat {
    Unknown,
    SmartSdr,
    Chirp,
};

struct MemoryCsvParseResult {
    QList<MemoryCsvRecord> records;
    QStringList errors;
    MemoryCsvFormat format{MemoryCsvFormat::Unknown};

    bool ok() const { return errors.isEmpty(); }
};

class MemoryCsvCompat
{
public:
    static QByteArray serialize(const QList<MemoryCsvRecord>& records);

    // Auto-detects the dialect from the header row: the native SmartSDR memory
    // CSV, or a CHIRP-next generic CSV export (first column "Location"). CHIRP
    // rows are field-mapped and unit-scaled into MemoryEntry on the way in.
    static MemoryCsvParseResult parse(const QByteArray& bytes);

    static MemoryCsvRecord fromMemoryEntry(const MemoryEntry& memory);

    // Human-readable dialect name for UI/log/automation ("SmartSDR", "CHIRP",
    // "unknown").
    static QString formatName(MemoryCsvFormat format);
};

} // namespace AetherSDR
