#pragma once

#include "core/backends/RadioStatusObservation.h"
#include <QMap>

namespace AetherSDR {

// True for the objects the radio decoder owns: "radio", "radio oscillator",
// "radio static_net_params" and "client 0x…".
bool isFlexRadioStatusObject(const QString& object);

// A radio-global, oscillator, static-network or client status → its
// observation, bounded field by field; nullopt for a malformed one. The status
// epoch is left for the caller to stamp.
std::optional<RadioStatusObservation> decodeFlexRadioStatus(
    const QString& object, const QMap<QString, QString>& fields);

} // namespace AetherSDR
