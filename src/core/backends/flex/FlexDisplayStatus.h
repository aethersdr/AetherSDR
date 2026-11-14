#pragma once

#include "core/backends/DisplayObservation.h"
#include <QMap>
#include <optional>

namespace AetherSDR {

// A "display pan|waterfall <id> …" status → its observation, bounded and
// identity-checked; nullopt for any other object or a malformed identity.
std::optional<DisplayObservation> decodeFlexDisplayStatus(
    const QString& object, const QMap<QString, QString>& fields);

} // namespace AetherSDR
