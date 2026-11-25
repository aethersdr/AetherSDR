#pragma once

#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace AetherSDR::aetherd {

// Pure translations between the engine's v1 resources
// (aetherd-control-resource-v1-catalogue.md) and the seam's existing types
// (docs/aetherd-remote-backend.md, *Mapping*). No new delta or signal type.

// "7" -> 7; nullopt for anything that is not a canonical non-negative integer.
[[nodiscard]] std::optional<int> canonicalSliceId(const QString& id);

// The full slice state the model needs, including the fields its receive gates
// read (panId, audio gain and mute, AGC). txSlice is always false: a remote
// engine is receive-only here. modes, when non-empty, becomes the mode list.
[[nodiscard]] SliceDelta sliceDeltaFrom(const QJsonObject& slice, const QStringList& modes);

struct PanState {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};
    std::optional<int> rfGain;
    std::optional<double> minimumBandwidthMhz;
    std::optional<double> maximumBandwidthMhz;
};
[[nodiscard]] std::optional<PanState> panStateFrom(const QJsonObject& panadapter);

// The engine's radioSession capability records, as this family's capabilities.
// Everything the engine does not send stays absent. Never transmit. agcMode is
// the engine's current AGC mode for the active slice: v1 has no AGC-mode
// method, so it is the only mode offered.
[[nodiscard]] RadioCapabilities capabilitiesFrom(const QJsonObject& radioSession,
                                                 const QString& agcMode);

// The resource-cache key for a v1 resource address.
[[nodiscard]] QString resourceKey(const QString& type, const QString& radioSession, const QString& id);
[[nodiscard]] QString resourceKey(const QJsonObject& address);

} // namespace AetherSDR::aetherd
