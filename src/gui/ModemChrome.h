#pragma once

#include <QColor>
#include <QString>

class QWidget;

namespace AetherSDR::ModemChrome {

// AetherModem window chrome, shared so other windows can match it: navy ground,
// gradient panels at 7 px radius, one green accent. Colours are ThemeManager
// token placeholders, so apply via ThemeManager::applyStyleSheet() (plain
// setStyleSheet() would paint nothing). Styled names: QFrame#ControlsFrame
// (gradient control group), #StatusFrame (foot strip), #ControlCell (labelled
// column); QLabel#SectionLabel (11 px caps heading), #StatusValue, #StatusDot
// (12 px health dot); QPushButton[chrome="tab"] (checked = green), #IconButton.
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

// Two palettes for the same sheet. Modem is the AetherModem window's own navy
// and green, which the docked applets keep. Canon is the style guide's (RFC
// #6226), for content inside a canon window (AetherRX, AetherTX): canon
// surfaces and ink, cyan selection and focus, and the painted canon check
// boxes and radio buttons (CanonIndicators.h). A Canon sheet embeds theme-
// dependent indicator images, so apply it through applyCanonSheet().
enum class Look { Modem, Canon };

QString styleSheet(Scale scale, Look look = Look::Modem);

// Resolve one of the Colour placeholders to a real colour, for painting code.
// The constants are {{token}} strings so a stylesheet can carry them through
// ThemeManager::applyStyleSheet(); a QPainter cannot, and QColor given a
// placeholder is simply invalid — it draws black, silently. One accessor so
// both paths name the same token rather than keeping a second palette for the
// half of the chrome that is painted by hand.
QColor colour(const char* placeholder);
// The same, resolved through `scope`'s theme container, so painted chrome
// inside a canon window takes the canon scope's colours.
QColor colour(const char* placeholder, const QWidget* scope);

} // namespace AetherSDR::ModemChrome
