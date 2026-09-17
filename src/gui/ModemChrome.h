#pragma once

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
namespace Colour {
inline constexpr const char* Background   = "#07101c";
inline constexpr const char* PanelTop     = "#111d2c";
inline constexpr const char* PanelBottom  = "#0a1421";
inline constexpr const char* Border       = "#233246";
inline constexpr const char* BorderSoft   = "#1c2a3b";
inline constexpr const char* ControlBorder= "#26374e";
inline constexpr const char* Text         = "#aeb9cc";
inline constexpr const char* TextBright   = "#d6dfeb";
inline constexpr const char* TextDim      = "#6e7a8d";
inline constexpr const char* Section      = "#8d99ad";
inline constexpr const char* StatusValue  = "#b9c4d7";
inline constexpr const char* Field        = "#050b13";
inline constexpr const char* FieldText    = "#c4cedd";
inline constexpr const char* Green        = "#64d36e";
inline constexpr const char* GreenEdge    = "#54c768";
inline constexpr const char* GreenBright  = "#80ed91";
inline constexpr const char* Amber        = "#d2a448";
} // namespace Colour

// Two sizes of the same chrome. Dialog is the modem's own 14 px scale; Compact
// is the docked-applet fit, which has ~280 px of width to spend and so shrinks
// fonts, padding and indicator sizes without changing any colour.
enum class Scale { Dialog, Compact };

QString styleSheet(Scale scale);

} // namespace AetherSDR::ModemChrome
