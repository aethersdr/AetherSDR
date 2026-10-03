#pragma once

#include <QString>

namespace AetherSDR {

// True when a slice draws RTTY mark/space tone cues, which replace the carrier
// marker (in RTTY the RF frequency is the mark). DIGL is excluded: it carries
// non-FSK modes (FT8, PSK31, VARA, ...), matching
// MainWindow::refreshRttyDecodeState(), and must keep its carrier marker (#5097).
// Own header so it is testable without SpectrumWidget.h.
inline bool drawsRttyToneCues(const QString& mode)
{
    return mode == QLatin1String("RTTY");
}

} // namespace AetherSDR
