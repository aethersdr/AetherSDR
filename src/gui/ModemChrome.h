#pragma once

#include <QColor>
#include <QString>

namespace AetherSDR::ModemChrome {

// The AetherModem window's visual language, factored out so a second window can
// wear it without copying the sheet.
//
// The palette is deliberately literal rather than theme-token driven: this is
// one specific chrome — near-black navy ground, gradient panels at 7 px radius,
// a single green accent — not the app's general surface styling, and a theme
// switch that recoloured it would make the two windows stop matching. The
// source of truth is kAetherModemStyle in Ax25HfPacketDecodeDialog.cpp; the
// values below were lifted from it unchanged, and the modem still carries its
// own copy (folding it onto this header is a separate, mechanical change).
//
// Structural object names the sheet styles, so a page reads the same way here
// as it does in the modem:
//
//   QFrame#ControlsFrame   a group of controls, gradient panel
//   QFrame#StatusFrame     the status strip at the foot of the window
//   QFrame#ControlCell     one labelled column inside a ControlsFrame
//   QLabel#SectionLabel    the 11 px all-caps heading above a cell's controls
//   QLabel#StatusValue     a status strip reading
//   QLabel#StatusDot       the 12 px round health indicator
//   QPushButton[chrome="tab"]  one tab in the top strip; checked draws green
//   QPushButton#IconButton a square, flat, icon-only button
//
// Every colour is a ThemeManager token placeholder, not a literal, so the
// chrome follows the loaded theme instead of pinning one palette. The sheet
// therefore has to be applied through ThemeManager::applyStyleSheet(), which
// resolves the {{...}} and re-resolves them when the theme changes; plain
// setStyleSheet() would install the placeholders verbatim and paint nothing.
//
// These were literals until the hardcoded-colour ratchet caught them, and the
// migration is why the panels read slightly differently: close variants
// collapse onto one token, which is the point — three greens a hover apart
// were three more unique colours for no gain the eye could find.
namespace Colour {
inline constexpr const char* Background   = "{{color.background.0}}";
inline constexpr const char* PanelTop     = "{{color.background.1}}";
inline constexpr const char* PanelBottom  = "{{color.background.0}}";
inline constexpr const char* Border       = "{{color.border.strong}}";
inline constexpr const char* BorderSoft   = "{{color.border.subtle}}";
inline constexpr const char* ControlBorder= "{{color.border.strong}}";
inline constexpr const char* Text         = "{{color.text.secondary}}";
inline constexpr const char* TextBright   = "{{color.text.primary}}";
inline constexpr const char* TextDim      = "{{color.text.label}}";
inline constexpr const char* Section      = "{{color.text.secondary}}";
inline constexpr const char* StatusValue  = "{{color.text.primary}}";
inline constexpr const char* Field        = "{{color.background.spectrum}}";
inline constexpr const char* FieldText    = "{{color.text.primary}}";
inline constexpr const char* Green        = "{{color.accent.success}}";
inline constexpr const char* GreenEdge    = "{{color.accent.success}}";
inline constexpr const char* GreenBright  = "{{color.accent.success}}";
inline constexpr const char* Amber        = "{{color.accent.warning}}";
} // namespace Colour

// Two sizes of the same chrome. Dialog is the modem's own 14 px scale; Compact
// is the docked-applet fit, which has ~280 px of width to spend and so shrinks
// fonts, padding and indicator sizes without changing any colour.
enum class Scale { Dialog, Compact };

QString styleSheet(Scale scale);

// Resolve one of the Colour placeholders to a real colour, for painting code.
// The constants are {{token}} strings so a stylesheet can carry them through
// ThemeManager::applyStyleSheet(); a QPainter cannot, and QColor given a
// placeholder is simply invalid — it draws black, silently. One accessor so
// both paths name the same token rather than keeping a second palette for the
// half of the chrome that is painted by hand.
QColor colour(const char* placeholder);

} // namespace AetherSDR::ModemChrome
