#pragma once

// The AGC-T slider's value as the RX applet and the slice flag print it in
// their tooltips. The slider is the 0..100 knob; where the backend declares
// the gain the knob sets (RadioCapabilities::agcGainScale), the tooltip shows
// that gain. Without one it shows the knob position with "dB", as before.
// Callers pass no scale for an external receiver (KiwiSDR): its slider is in
// dB already.

#include "core/backends/RadioCapabilities.h"

#include <QString>

#include <optional>

namespace AetherSDR {

inline QString agcKnobText(int knob, const std::optional<RadioCapabilities::AgcGainScale>& scale)
{
    if (!scale) {
        return QStringLiteral("%1 dB").arg(knob);
    }
    return QStringLiteral("%1 dB").arg(scale->offsetDb + scale->dbPerStep * knob, 0, 'f', 1);
}

} // namespace AetherSDR
