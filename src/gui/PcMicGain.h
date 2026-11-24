#pragma once

#include "core/AppSettings.h"
#include "models/TransmitModel.h"

#include <QObject>

#include <utility>

namespace AetherSDR {

// The client-side PC mic gain (0-100) for the connected radio's microphone
// path. The saved PcMicGain applies only to a PC input whose level the client
// owns. Unity for a hardware mic, and on a host-modulating radio, whose MIC
// slider sets the modulator's gain and cannot move the saved value.
inline int pcMicGainPercent(const TransmitModel& tx)
{
    if (tx.hostModulation() || tx.micSelection() != QLatin1String("PC")) {
        return 100;
    }
    return AppSettings::instance().value("PcMicGain", 100).toInt();
}

// Re-applies it on every mic state change and whenever host modulation begins
// or ends, so a family swap leaves neither radio with the other's gain. `tx`
// must outlive `context`.
template <typename Apply>
void bindPcMicGain(const TransmitModel& tx, const QObject* context, Apply apply)
{
    QObject::connect(&tx, &TransmitModel::micStateChanged, context,
                     [&tx, apply] { apply(pcMicGainPercent(tx)); });
    QObject::connect(&tx, &TransmitModel::hostModulationChanged, context,
                     [&tx, apply = std::move(apply)](bool) { apply(pcMicGainPercent(tx)); });
}

} // namespace AetherSDR
