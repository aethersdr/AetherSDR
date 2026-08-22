#pragma once

#include <QString>

namespace AetherSDR {

// True when a slice should draw RTTY mark/space tone cues on the panadapter,
// and when the VFO flag must therefore stay clear of the region left of the
// carrier where those cues live.
//
// DIGL is deliberately NOT included. It is a general lower-sideband *data* mode
// carrying FT8, JS8, PSK31, VARA, SSTV and RADE, none of which are FSK, so
// painting mark/space tone markers on them is meaningless. This is the same
// reasoning MainWindow::refreshRttyDecodeState() already applies when it
// refuses to open a Baudot decoder on a DIGL slice.
//
// Treating DIGL as RTTY also *suppressed the carrier marker entirely*, because
// the tone cues were drawn instead of the centre line rather than on top of it
// (#5097). The carrier is now unconditional; this predicate only adds cues.
//
// Lives in its own header so the rule is unit-testable without dragging in
// SpectrumWidget.h (which pulls QRhiWidget and the DSS renderer).
inline bool drawsRttyToneCues(const QString& mode)
{
    return mode == QLatin1String("RTTY");
}

} // namespace AetherSDR
