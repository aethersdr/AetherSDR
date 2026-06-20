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
inline const QColor kControl{QStringLiteral("#161620ff")};    // control body — TBD
inline const QColor kBackground{QStringLiteral("#5ec4eaff")}; // recessed hole
inline const QColor kForeground{QStringLiteral("#fe4343")}; // indicator bar
inline const QColor kIndicator{QStringLiteral("#ffffff")};  // reserved (future)
inline const QColor kExtreme{QStringLiteral("#ffffff")};    // reserved (future)
inline const QColor kShadow{0, 0, 0, 70};                   // inset shadow — TBD
} // namespace SmartMtrColors

// Geometry of the SmartMTR control, in UNITS.
namespace SmartMtrUnits {
// The control's full design area.
inline constexpr double kControlW = 250.0;
inline constexpr double kControlH = 40.0;

// The recessed "hole" / indicator area: horizontally centered, fixed 20 units
// from the top.
inline constexpr double kHoleW = 240.0;
inline constexpr double kHoleH = 10.0;
inline constexpr double kHoleMargX = (kControlW - kHoleW) / 2.0; // 10, centered
inline constexpr double kHoleMargY = 20.0;                       // from top

// Corner radius of the hole (and, concentrically, its inset rim), in units.
inline constexpr double kHoleRadius = 4.0;

// Inset shadow rim width, equal on all four inner sides of the hole.
inline constexpr double kShadow = 2.0;

// Indicator bar fill, as a fraction of the hole width. Hard-pinned for now;
// later steps map a measured level onto this.
inline constexpr double kIndicatorFraction = 0.5; // 50%

// Thickness of the bright marker line sitting on top of the bar's right end
// (kIndicator). Right-aligned within the bar, so it never extends past it.
inline constexpr double kIndicatorLine = 1.0;
} // namespace SmartMtrUnits

} // namespace AetherSDR
