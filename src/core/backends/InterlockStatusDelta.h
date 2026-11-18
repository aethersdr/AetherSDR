#pragma once

#include "core/backends/TransmitDelta.h"

#include <QMetaType>
#include <QString>

#include <optional>

namespace AetherSDR {

// One "interlock …" status: who holds the transmitter, the interlock state
// and its PTT source, why TX is denied, and the interlock timing fields.
// State, source and reason are upper-cased; everything is present-only
// except tx_allowed and reason, which read as empty when absent.
struct InterlockStatusDelta {
    std::optional<quint32> txClientHandle;
    std::optional<QString> state;
    std::optional<QString> source;
    QString txAllowed;
    QString reason;
    TransmitDelta timing;   // delays, timeout, TX request polarities
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::InterlockStatusDelta)
