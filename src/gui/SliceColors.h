#pragma once

// Per-slice colour catalogue size used by SpectrumWidget, VfoWidget,
// RxApplet, SliceColorManager, and RadioSetupDialog to size arrays and
// loop indices.  The actual hex values now live in the theme JSON under
// `color.slice.{a..h}` (active) and `color.slice.dim.{a..h}` (dim);
// resolve them through SliceColorManager so they pick up theme changes.

namespace AetherSDR {

inline constexpr int kSliceColorCount = 8;

} // namespace AetherSDR
