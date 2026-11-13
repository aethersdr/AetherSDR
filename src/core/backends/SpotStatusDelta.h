#pragma once

#include <QMap>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// One radio spot status ("spot N …"). The record is SpotModel's own spot
// record (callsign, rx_freq, mode, color, …), the same form every other spot
// producer writes, so one parser keeps every source consistent.
struct SpotStatusDelta {
    enum class Kind { Status, Removed, Triggered };
    Kind kind{Kind::Status};
    int index{-1};
    QString pan;                    // Triggered: the pan the operator clicked
    QMap<QString, QString> record;  // Status
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SpotStatusDelta)
