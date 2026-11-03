#pragma once

#include "core/MemoryFieldValues.h"
#include "models/MemoryEntry.h"
#include <QSet>
#include <QMetaType>
#include <QString>

#include <cmath>
#include <optional>

namespace AetherSDR {

enum class SlicePresetField { Step, RepeaterOffset, ToneMode, ToneValue };
struct SlicePresetControl {
    QSet<SlicePresetField> fields;
    bool supports(SlicePresetField field) const { return fields.contains(field); }
};

// A net's optional slice corrections, not a radio memory-slot operation.
struct SlicePresetRequest {
    std::optional<int> stepHz;
    std::optional<QString> direction, toneMode;
    std::optional<double> offsetMhz, toneHz;
    bool empty() const { return !stepHz && !direction && !toneMode && !offsetMhz && !toneHz; }
    bool valid() const
    {
        return !empty() && (!stepHz || *stepHz > 0)
            && (!direction || (!direction->isEmpty() && MemoryFields::offsetDirToWire(*direction) == *direction))
            && (!toneMode || (!toneMode->isEmpty() && MemoryFields::toneModeToWire(*toneMode) == *toneMode))
            && (bool(direction) == bool(offsetMhz))
            && (!offsetMhz || (std::isfinite(*offsetMhz) && *offsetMhz >= 0.0 && *offsetMhz <= 1000.0))
            && (!toneHz || (std::isfinite(*toneHz) && *toneHz >= 0.0 && *toneHz <= 1000.0));
    }
    static SlicePresetRequest repeaterFixup(const MemoryEntry& entry)
    {
        SlicePresetRequest request;
        const QString direction = entry.offsetDir.trimmed().toLower();
        if (direction == QLatin1String("up") || direction == QLatin1String("down")
            || direction == QLatin1String("simplex")) {
            request.direction = direction;
            request.offsetMhz = std::abs(entry.repeaterOffset);
        }
        if (!entry.toneMode.trimmed().isEmpty()) { request.toneMode = entry.toneMode.trimmed().toLower(); }
        if (request.toneMode || entry.toneValue > 0.0) { request.toneHz = entry.toneValue; }
        return request;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SlicePresetRequest)
