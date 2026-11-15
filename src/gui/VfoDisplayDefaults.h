#pragma once

namespace AetherSDR {

// Global defaults for the VFO marker and filter-edge appearance (View > VFO
// Marker Size / VFO Filter Edge, #5570); slices without a per-slice override
// (set by the VFO flag buttons) follow these. Separate from VfoWidget so it is
// testable socket-free. Stored as one object under root key "VfoDisplayDefaults".
namespace VfoDisplayDefaults {

// Snap to one of the supported states: 0 (off), 1, 3.
int  normalizeMarkerWidth(int widthPx);

int  markerWidth();
bool filterEdgesHidden();

void setMarkerWidth(int widthPx);
void setFilterEdgesHidden(bool hide);

} // namespace VfoDisplayDefaults
} // namespace AetherSDR
