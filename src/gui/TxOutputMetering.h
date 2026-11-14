#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QString>

namespace AetherSDR {

// Whether the TX applet's forward-power and SWR gauges measure the selected
// TX output (#4954). A backend lists the outputs it cannot meter in
// IndependentTxAntenna::unmeteredOutputs (the HL2's RF1). On such an output
// the gauges would sit at about 0 W and 1.0, which reads as "nothing is coming
// out" when nothing is being measured. No radio connected, no record, or an
// output not on the list is measured, so every other backend and the HL2's
// ANT keep the gauges exactly as before. Presentation only: keying, drive and
// the route are untouched by it.
inline bool txOutputIsMetered(bool connected, const RadioCapabilities& caps,
                              const QString& txOutput)
{
    if (!connected || !caps.independentTxAntenna || txOutput.isEmpty()) {
        return true;
    }
    return !caps.independentTxAntenna->unmeteredOutputs.contains(txOutput);
}

// The reason both gauges carry, visibly and in accessibleDescription, while
// the output is not metered.
inline QString txOutputNotMeteredReason(const QString& txOutput)
{
    return QStringLiteral("Not measured on %1: the radio has no power detector "
                          "on this output, so the power and SWR gauges cannot "
                          "show what %1 sends. A reading near 0 W here is not "
                          "a fault.").arg(txOutput);
}

// The text drawn inside a gauge bar while its output is not metered.
inline QString txOutputNotMeteredLabel(const QString& gaugeLabel, const QString& txOutput)
{
    return QStringLiteral("%1: not measured on %2").arg(gaugeLabel, txOutput);
}

} // namespace AetherSDR
