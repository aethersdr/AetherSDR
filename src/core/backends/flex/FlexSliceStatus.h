#pragma once

#include "core/backends/SliceObservation.h"

#include <QMap>
#include <QString>

namespace AetherSDR {

// Stateless decoding is shared by LAN, WAN and injected protocol tests.
namespace FlexSliceStatus {
bool isSlice(const QString& object);
std::optional<SliceObservation> decode(const QString& object,
                                      const QMap<QString, QString>& fields);
SliceDelta decodeValues(const QMap<QString, QString>& fields);
}

} // namespace AetherSDR
