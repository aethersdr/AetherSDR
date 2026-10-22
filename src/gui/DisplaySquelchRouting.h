#pragma once
#include "ModeFilterPresets.h"
#include "models/SliceModel.h"
#include "models/RadioModel.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace AetherSDR::DisplaySquelchRouting {
inline std::optional<SquelchLevelScale> scaleForSlice(
    const SliceModel* slice, const RadioModel& radio)
{
    const auto caps = radio.backendCapabilities();
    if (!slice || !radio.isConnected() || !ModeFilters::squelchAvailableInMode(slice->mode(),
            caps.squelchLevelScale ? &*caps.squelchLevelScale : nullptr,
            caps.hasModeIndependentSquelch, slice->externalReceiveReplacementActive())) {
        return std::nullopt;
    }
    return squelchScaleForMode(caps.squelchLevelScale, slice->mode());
}

inline bool tracks(const SliceModel* slice, const QString& panId,
                   const RadioModel& radio)
{
    const auto scale = scaleForSlice(slice, radio);
    return slice && slice->panId() == panId && slice->displayAutoSquelch()
        && !slice->externalReceiveReplacementActive() && scale
        && !scale->automaticInEngine && autoSquelchAvailable(scale);
}

// A pan has one SQL overlay: prefer its selected slice, then its title slice.
inline SliceModel* displaySlice(const QList<SliceModel*>& slices,
                               const QString& panId, const SliceModel* active, int titleId)
{
    SliceModel* fallback = nullptr;
    SliceModel* title = nullptr;
    for (SliceModel* slice : slices) {
        if (slice->panId() != panId) { continue; }
        if (slice == active) { return slice; }
        if (!fallback) { fallback = slice; }
        if (slice->sliceId() == titleId) { title = slice; }
    }
    return title ? title : fallback;
}

inline void applyFloor(const QList<SliceModel*>& slices, const QString& panId,
                       const RadioModel& radio, float floor)
{
    if (!std::isfinite(floor) || floor <= -500.0f) { return; }
    for (SliceModel* slice : slices) {
        if (!tracks(slice, panId, radio)) { continue; }
        const auto scale = scaleForSlice(slice, radio);
        const int margin = slice->displayAutoSquelchMarginDb().value_or(10);
        const int level = static_cast<int>(std::clamp(
            (floor + margin - scale->offsetDb) / scale->dbPerStep + 0.5, 1.0, 100.0));
        slice->applyDisplayAutoSquelchLevel(level);
    }
}
} // namespace AetherSDR::DisplaySquelchRouting
