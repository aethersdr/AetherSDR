#pragma once

#include "core/backends/RadioControlTypes.h"
#include "core/MemoryFieldValues.h"
#include "models/MemoryEntry.h"

#include <cmath>
#include <algorithm>
#include <functional>
#include <optional>

namespace AetherSDR {

// Present fields only: editing a name must not overwrite native channel metadata.
struct MemoryPatch {
    static constexpr int kMaxStoredFilterEdgeHz = 100000;
    std::optional<QString> group, owner, name, mode, offsetDir, toneMode;
    std::optional<double> freq, repeaterOffset, toneValue;
    std::optional<int> step, squelchLevel, rxFilterLow, rxFilterHigh;
    std::optional<int> rttyMark, rttyShift, diglOffset, diguOffset;
    std::optional<bool> squelch;

    bool empty() const
    {
        return !group && !owner && !name && !mode && !offsetDir && !toneMode
            && !freq && !repeaterOffset && !toneValue && !step && !squelchLevel
            && !rxFilterLow && !rxFilterHigh && !rttyMark && !rttyShift
            && !diglOffset && !diguOffset && !squelch;
    }
    bool valid() const
    {
        for (const std::optional<QString>* text : {&group, &owner, &name}) {
            if (*text && ((*text)->size() > 256
                || MemoryFields::sanitizeText(**text) != **text)) { return false; }
        }
        return !empty()
            && (!mode || MemoryFields::isStorableMode(*mode))
            && (!offsetDir || !MemoryFields::offsetDirToWire(*offsetDir).isEmpty())
            && (!toneMode || !MemoryFields::toneModeToWire(*toneMode).isEmpty())
            && (!freq || (std::isfinite(*freq) && *freq > 0.0))
            && (!repeaterOffset || (std::isfinite(*repeaterOffset) && *repeaterOffset >= 0.0))
            && (!toneValue || (std::isfinite(*toneValue) && *toneValue >= 0.0))
            && (!step || *step > 0)
            && (!squelchLevel || (*squelchLevel >= 0 && *squelchLevel <= 100))
            // Storage covers the existing WFM +/-100 kHz envelope; applying
            // filters is still constrained by the receiving backend.
            && (!rxFilterLow || (*rxFilterLow >= -kMaxStoredFilterEdgeHz && *rxFilterLow <= kMaxStoredFilterEdgeHz))
            && (!rxFilterHigh || (*rxFilterHigh >= -kMaxStoredFilterEdgeHz && *rxFilterHigh <= kMaxStoredFilterEdgeHz))
            && (!rttyMark || (*rttyMark >= 100 && *rttyMark <= 4000))
            && (!rttyShift || (*rttyShift >= 0 && *rttyShift <= 4000))
            && (!diglOffset || (*diglOffset >= -12000 && *diglOffset <= 12000))
            && (!diguOffset || (*diguOffset >= -12000 && *diguOffset <= 12000));
    }
    static MemoryPatch fromEntry(const MemoryEntry& entry)
    {
        MemoryPatch patch;
        patch.group = entry.group; patch.owner = entry.owner; patch.name = entry.name;
        patch.mode = entry.mode; patch.freq = entry.freq; patch.step = entry.step;
        patch.offsetDir = entry.offsetDir.isEmpty() ? QStringLiteral("simplex") : entry.offsetDir;
        patch.repeaterOffset = std::abs(entry.repeaterOffset);
        patch.toneMode = entry.toneMode.isEmpty() ? QStringLiteral("off") : entry.toneMode;
        patch.toneValue = entry.toneValue;
        patch.squelch = entry.squelch; patch.squelchLevel = entry.squelchLevel;
        patch.rxFilterLow = entry.rxFilterLow; patch.rxFilterHigh = entry.rxFilterHigh;
        patch.rttyMark = entry.rttyMark; patch.rttyShift = entry.rttyShift;
        patch.diglOffset = entry.diglOffset; patch.diguOffset = entry.diguOffset;
        return patch;
    }
    MemoryEntry appliedTo(MemoryEntry entry) const
    {
        if (group) { entry.group = *group; }
        if (owner) { entry.owner = *owner; }
        if (name) { entry.name = *name; }
        if (mode) {
            entry.mode = MemoryFields::modeToWire(*mode);
            if (entry.nativeFilter > 0) {
                const bool data = entry.mode == QLatin1String("DIGU")
                    || entry.mode == QLatin1String("DIGL") || entry.mode == QLatin1String("DFM");
                entry.dataMode = data ? std::max(1, entry.dataMode) : 0;
            }
        }
        if (freq) { entry.freq = *freq; }
        if (step) { entry.step = *step; }
        if (offsetDir) { entry.offsetDir = MemoryFields::offsetDirToWire(*offsetDir); }
        if (repeaterOffset) { entry.repeaterOffset = *repeaterOffset; }
        if (toneMode) { entry.toneMode = MemoryFields::toneModeToWire(*toneMode); }
        if (toneValue) { entry.toneValue = *toneValue; }
        if (squelch) { entry.squelch = *squelch; }
        if (squelchLevel) { entry.squelchLevel = *squelchLevel; }
        if (rxFilterLow) { entry.rxFilterLow = *rxFilterLow; }
        if (rxFilterHigh) { entry.rxFilterHigh = *rxFilterHigh; }
        if (rttyMark) { entry.rttyMark = *rttyMark; }
        if (rttyShift) { entry.rttyShift = *rttyShift; }
        if (diglOffset) { entry.diglOffset = *diglOffset; }
        if (diguOffset) { entry.diguOffset = *diguOffset; }
        return entry;
    }
};

struct MemoryRequest {
    enum class Action { Create, Update, Remove, Recall };
    Action action;
    int index{-1};
    MemoryPatch patch;
    int sliceId{-1};
    std::optional<MemoryEntry> recall{};
    std::optional<quint64> expectedRevision{};
    bool valid() const
    {
        if (action < Action::Create || action > Action::Recall) { return false; }
        if (action == Action::Create) {
            return index == -1 && patch.empty() && sliceId == -1 && !recall && !expectedRevision;
        }
        if (action != Action::Recall && (sliceId != -1 || recall)) { return false; }
        if (action == Action::Recall && ((sliceId >= 0) != bool(recall) || sliceId < -1)) { return false; }
        return index >= 0 && (action == Action::Update ? patch.valid() : patch.empty())
            && (!recall || (action == Action::Recall && sliceId >= 0
                && std::isfinite(recall->freq) && recall->freq >= 0.0
                && std::isfinite(recall->repeaterOffset) && std::isfinite(recall->toneValue)));
    }
};
using MemoryCreateCompletion = std::function<void(const RadioControlReply&, int)>;

} // namespace AetherSDR
