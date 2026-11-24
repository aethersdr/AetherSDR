#pragma once

#include "core/backends/SetupStatusObservation.h"
#include <QMap>

namespace AetherSDR {

// True for the objects the setup decoder owns: "xvtr", "usb_cable",
// "license" and "radio filter_sharpness".
bool isFlexSetupStatusObject(const QString& object);

// A setup status → its observation, bounded field by field; nullopt for a
// malformed one. The status epoch is left for the caller to stamp.
std::optional<SetupStatusObservation> decodeFlexSetupStatus(
    const QString& object, const QMap<QString, QString>& fields);

} // namespace AetherSDR
