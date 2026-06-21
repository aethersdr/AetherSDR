#pragma once

#include <QColor>

namespace AetherSDR {

// ============================================================================
// SmartMTR design tokens
// ============================================================================
//
// Single source of truth for the SmartMTR control's look. To re-tune the
// control's proportions or palette, edit THIS file only — no drawing code
// references magic numbers; every element reads from these constants.
//
// Geometry is expressed in UNITS (see SmartMtrGeometry.h for the UNITS->pixel
// mapping). UNITS are abstract, resolution-independent design units that fix
// only the proportions of the control, never its pixel size.
// ============================================================================

// Palette for the SmartMTR meter control.
namespace SmartMtrColors {
inline const QColor kControl{QStringLiteral("#161620")};      // = slice flag bg
// (VfoWidget base #0a0a14 + its 5% white depth overlay, composited)
inline const QColor kBackground{QStringLiteral("#5ec4eaff")}; // recessed hole
inline const QColor kForeground{QStringLiteral("#ff4444")}; // indicator bar — red
inline const QColor kIndicator{QStringLiteral("#ffffff")};  // value end-line
inline const QColor kExtreme{QStringLiteral("#ffffff")};    // reserved (future)
inline const QColor kShadow{0, 0, 0, 100};                   // inset shadow — TBD
inline const QColor kMarkerNormal{QStringLiteral("#5ec4ea")}; // scale tick — normal
inline const QColor kMarkerHigh{QStringLiteral("#ff4444")};   // scale tick — high
} // namespace SmartMtrColors

// Geometry of the SmartMTR control, in UNITS.
namespace SmartMtrUnits {
// The control's full design area.  The height hugs the actual content extent —
// the bottom ticks reach kHoleMargY(20) + kHoleH(10) + kMarkerLargeH(5) = 35
// units — so the widget reserves no empty band below the meter.  That keeps the
// gap to the tab row below minimal (matching the S-meter), rather than padding
// it out with leftover canvas.  Width-driven scale means this only trims the
// bottom; the bar/hole/ticks render at the same size. (#SmartMTR)
inline constexpr double kControlW = 250.0;
inline constexpr double kControlH = 35.0;

// The recessed "hole" / indicator area: horizontally centered, fixed 20 units
// from the top.
inline constexpr double kHoleW = 240.0;
inline constexpr double kHoleH = 10.0;
inline constexpr double kHoleMargX = (kControlW - kHoleW) / 2.0; // 10, centered
inline constexpr double kHoleMargY = 20.0;                       // from top

// Corner radius of the hole (and, concentrically, its inset rim), in units.
inline constexpr double kHoleRadius = 2.0;

// Inset shadow depth: how far the soft gradient reaches inward from each inner
// edge of the hole (equal on all four sides).
inline constexpr double kShadow = 3.0;

// Indicator scale band, in hole-local units (0..kHoleW). The minimum/null value
// sits at kScaleMin, the maximum at kScaleMax, leaving a symmetric gap at each
// end of the hole. Markers outside [kScaleMin, kScaleMax] are not rendered.
inline constexpr double kScaleMin = 10.0;
inline constexpr double kScaleMax = 230.0;

// Thickness of the bright value line sitting on top of the bar's right end
// (kIndicator). Right-aligned within the bar, so it never extends past it.
inline constexpr double kIndicatorLine = 1.0;

// Scale-marker tick sizes (height = length away from the hole, width = thickness
// straddling the marker position).
inline constexpr double kMarkerSmallH = 4.0, kMarkerSmallW = 1.0;
inline constexpr double kMarkerLargeH = 5.0, kMarkerLargeW = 2.0;
// Small ticks are drawn a bit transparent so they read as secondary to the
// large (labeled) ones.
inline constexpr double kMarkerSmallOpacity = 0.6;

// Marker labels (top ticks only): font cell height and gap above the tick.
// Above-hole budget is kHoleMargY(20) − kMarkerLargeH(5) − kLabelGap(1) = 14
// units, so up to ~12 fits with a small top margin. Strong labels use the full
// height; normal labels are minimally smaller.
inline constexpr double kLabelHeight = 12.0;       // strong
inline constexpr double kLabelHeightNormal = 11.0; // normal (slightly smaller)
inline constexpr double kLabelGap = 1.0;
} // namespace SmartMtrUnits

} // namespace AetherSDR
