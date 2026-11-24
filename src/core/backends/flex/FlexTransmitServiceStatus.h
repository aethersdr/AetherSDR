#pragma once

#include "core/backends/TransmitServiceStatus.h"
#include <QMap>

namespace AetherSDR {

// "dvk …" and "navtex …" status → their observations; nullopt for any other
// object or a malformed report. The status epoch is left for the caller.
std::optional<DvkStatusObservation> decodeFlexDvkStatus(const QString& object, const QMap<QString, QString>& fields);
std::optional<NavtexStatusObservation> decodeFlexNavtexStatus(const QString& object, const QMap<QString, QString>& fields);

} // namespace AetherSDR
