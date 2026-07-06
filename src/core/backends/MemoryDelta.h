#pragma once

#include <optional>

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized memory-slot status delta (aetherd RFC 2.3 — RadioModel residual),
// keyed by slot index. The backend decodes the vendor memory-status kv-set:
// present-only, ok-guarded numerics, and `removed` set when the wire signalled
// the slot is gone ("in_use=0" or a bare "removed"). Text fields are carried
// raw — RadioModel::applyMemoryChanges owns the space-encoding (0x7f→' ') +
// sanitisation (MemoryFields, a model concern) before writing MemoryEntry.
struct MemoryDelta {
    int  index{-1};
    bool removed{false};

    // Text (carried raw; model decodes/sanitises)
    std::optional<QString> group;
    std::optional<QString> owner;
    std::optional<QString> name;
    std::optional<QString> mode;
    std::optional<QString> offsetDir;   // wire key "repeater"
    std::optional<QString> toneMode;

    // Numeric
    std::optional<double>  freq;
    std::optional<double>  repeaterOffset;
    std::optional<double>  toneValue;
    std::optional<int>     step;
    std::optional<bool>    squelch;
    std::optional<int>     squelchLevel;
    std::optional<int>     rxFilterLow;
    std::optional<int>     rxFilterHigh;
    std::optional<int>     rttyMark;
    std::optional<int>     rttyShift;
    std::optional<int>     diglOffset;
    std::optional<int>     diguOffset;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::MemoryDelta)
