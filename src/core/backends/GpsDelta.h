#pragma once

#include <optional>

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized GPS status delta (aetherd RFC 2.3 — RadioModel residual). The
// backend tokenizes the vendor GPS status line and populates only the fields it
// reported; RadioModel::applyGpsChanges applies them and emits gpsStatusChanged.
// Positional/text fields keep the radio's string form (units included, e.g.
// "644 m", "0 kts", "0 ppb"); only the satellite counts are numeric.
struct GpsDelta {
    std::optional<QString> status;
    std::optional<int>     tracked;
    std::optional<int>     visible;
    std::optional<QString> grid;
    std::optional<QString> altitude;
    std::optional<QString> lat;
    std::optional<QString> lon;
    std::optional<QString> time;
    std::optional<QString> speed;
    std::optional<QString> track;
    std::optional<QString> freqError;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::GpsDelta)
