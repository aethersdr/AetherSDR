#pragma once

namespace AetherSDR {

// Global defaults for the VFO marker and filter-edge appearance (View ▸ VFO
// Marker Size / VFO Filter Edge, #5570). A slice that has never had a per-slice
// override follows these; the VFO flag buttons write the per-slice override.
//
// Its own translation unit rather than static members on VfoWidget: this is the
// feature's configuration object (Principle V), it is read at menu-build time
// and at every slice attach, and keeping it free of the widget lets it be
// exercised by a socket-free test without linking the whole VFO panel.
//
// Storage is one self-contained object under the single root key
// "VfoDisplayDefaults" — never loose flat keys (Constitution Principle V).
namespace VfoDisplayDefaults {

// Snap to one of the supported states: 0 (off), 1, 3.
int  normalizeMarkerWidth(int widthPx);

int  markerWidth();
bool filterEdgesHidden();

void setMarkerWidth(int widthPx);
void setFilterEdgesHidden(bool hide);

} // namespace VfoDisplayDefaults
} // namespace AetherSDR
