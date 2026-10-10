#include "ModemChrome.h"
#include "CanonIndicators.h"
#include "core/ThemeManager.h"

#include <QWidget>

#include <utility>

namespace AetherSDR::ModemChrome {

namespace {

// Every colour the sheet uses, in one place per look. Modem is the AetherModem
// window's own palette, base tokens and its navy literals, which the docked
// applets still wear. Canon is the style guide's (RFC #6226) for the windows
// that moved to it: content on canon raised over the window's ground, fields
// on control, selection and focus in canon cyan, status keeping its green.
struct Palette {
    const char* text; const char* ground; const char* panelTop; const char* panelBottom;
    const char* border; const char* borderSoft; const char* section; const char* textDim;
    const char* statusValue; const char* statusDot; const char* controlBorder;
    const char* accent; const char* accentBright; const char* textBright;
    const char* fieldText; const char* field;
    // The sheet's former literals, by role.
    std::pair<const char*, const char*> literals[24];
};

const Palette& palette(Look look)
{
    static const Palette kModem{
        Colour::Text, Colour::Background, Colour::PanelTop, Colour::PanelBottom,
        Colour::Border, Colour::BorderSoft, Colour::Section, Colour::TextDim,
        Colour::StatusValue, Colour::Green, Colour::ControlBorder,
        Colour::GreenEdge, Colour::GreenBright, Colour::TextBright,
        Colour::FieldText, Colour::Field,
        {{"@radioFill@", "#08111d"}, {"@radioFillChecked@", "#132d26"},
         {"@checkEdge@", "#34533c"}, {"@checkFill@", "#0d1a18"},
         {"@checkFillChecked@", "#5ebd69"}, {"@buttonText@", Colour::Text},
         {"@buttonTop@", "#142235"}, {"@buttonBottom@", "#0b1625"},
         {"@buttonHoverEdge@", "#3c526d"}, {"@disabledEdge@", "#1d2a3c"},
         {"@disabledFill@", "#0b1522"}, {"@tabCheckedText@", "#d4deea"},
         {"@tabCheckedFill@", "#0d1c20"}, {"@tabDisabledText@", "#7f8b9e"},
         {"@inputFill@", "#0b1625"}, {"@selection@", "#1b3650"},
         {"@sliderHandleDisabled@", "#2d3a45"}, {"@sliderFillDisabled@", "#22402a"},
         {"@tableText@", "#c2ccdb"}, {"@tableAlt@", "#081220"},
         {"@tableGrid@", "#14202f"}, {"@headerFill@", "#0d1825"},
         {"@scrollHandle@", "#25364d"},
         // Tabs take the button text colour here, so this adds nothing.
         {"@tabTextRule@", ""}}};
    static const Palette kCanon{
        "{{color.canon.inkSoft}}", "transparent", "{{color.canon.raised}}", "{{color.canon.raised}}",
        "{{color.canon.line}}", "{{color.canon.line}}", "{{color.canon.muted}}", "{{color.canon.muted}}",
        "{{color.canon.ink}}", Colour::Green, "{{color.canon.lineHi}}",
        "{{color.canon.cyan}}", "{{color.canon.aqua}}", "{{color.canon.aqua}}",
        "{{color.canon.ink}}", "{{color.canon.control}}",
        {{"@radioFill@", "{{color.canon.control}}"}, {"@radioFillChecked@", "{{color.canon.nested}}"},
         {"@checkEdge@", "{{color.canon.lineHi}}"}, {"@checkFill@", "{{color.canon.control}}"},
         {"@checkFillChecked@", "{{color.canon.cyan}}"}, {"@buttonText@", "{{color.canon.cyan}}"},
         {"@buttonTop@", "{{color.canon.control}}"}, {"@buttonBottom@", "{{color.canon.control}}"},
         {"@buttonHoverEdge@", "{{color.canon.aqua}}"}, {"@disabledEdge@", "{{color.canon.line}}"},
         {"@disabledFill@", "transparent"}, {"@tabCheckedText@", "{{color.canon.aqua}}"},
         {"@tabCheckedFill@", "{{color.canon.nested}}"}, {"@tabDisabledText@", "{{color.canon.muted}}"},
         {"@inputFill@", "{{color.canon.control}}"}, {"@selection@", "{{color.canon.nested}}"},
         {"@sliderHandleDisabled@", "{{color.canon.line}}"}, {"@sliderFillDisabled@", "{{color.canon.line}}"},
         {"@tableText@", "{{color.canon.inkSoft}}"}, {"@tableAlt@", "{{color.canon.raised}}"},
         {"@tableGrid@", "{{color.canon.line}}"}, {"@headerFill@", "{{color.canon.raised}}"},
         {"@scrollHandle@", "{{color.canon.lineHi}}"},
         // Canon buttons are cyan; a tab is a page name, so it reads in ink.
         {"@tabTextRule@", "\n    color: {{color.canon.inkSoft}};"}}};
    return look == Look::Canon ? kCanon : kModem;
}

} // namespace

QString styleSheet(Scale scale, Look look)
{
    const Palette& c = palette(look);
    const bool compact = (scale == Scale::Compact);

    // Everything that changes between the two scales, in one place: no colour
    // appears in this list, so the applet and the dialog cannot drift apart in
    // anything but size.
    const int  baseFont     = compact ? 11 : 14;
    const int  sectionFont  = compact ? 10 : 11;
    const int  tabFont      = compact ? 10 : 13;
    const int  fieldFont    = compact ? 11 : 13;
    const int  indicator    = compact ? 14 : 20;
    const int  indicatorRad = indicator / 2;
    const int  checkRadius  = compact ? 3 : 4;
    const int  spacing      = compact ? 5 : 9;
    const int  buttonPadV   = compact ? 4 : 10;
    const int  buttonPadH   = compact ? 8 : 18;
    const int  radius       = compact ? 5 : 7;
    const int  tabPadV      = compact ? 2 : 4;
    const int  tabPadH      = compact ? 4 : 12;
    const int  fieldPadV    = compact ? 4 : 10;
    const int  fieldPadH    = compact ? 6 : 12;
    const int  grooveHeight = compact ? 4 : 6;
    const int  handleSize   = compact ? 10 : 14;
    const int  iconButton   = compact ? 24 : 38;

    QString sheet = QStringLiteral(R"(
QWidget {
    color: %1;
    background: %2;
    font-size: %3px;
}
QLabel {
    background: transparent;
}
QFrame#ControlsFrame,
QFrame#StatusFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 %4, stop:1 %5);
    border: 1px solid %6;
    border-radius: %7px;
}
QFrame#TabsFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 %4, stop:1 %5);
    border: 1px solid %6;
    border-radius: %7px;
}
QFrame#ControlCell {
    background: transparent;
    border-right: 1px solid %8;
}
QFrame#ControlCellLast {
    background: transparent;
}
QLabel#SectionLabel {
    background: transparent;
    color: %9;
    font-size: %10px;
    font-weight: 700;
}
QLabel#SectionLabel:disabled {
    color: %11;
}
QLabel#StatusValue {
    background: transparent;
    color: %12;
    font-size: %3px;
    font-weight: 600;
}
QLabel#StatusDot {
    background: %13;
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QRadioButton,
QCheckBox {
    background: transparent;
    color: %1;
    spacing: %14px;
}
QRadioButton:disabled,
QCheckBox:disabled {
    color: %11;
}
QRadioButton::indicator {
    width: %15px;
    height: %15px;
    border-radius: %16px;
    border: 2px solid %17;
    background: @radioFill@;
}
QRadioButton::indicator:checked {
    border: 2px solid %18;
    background: @radioFillChecked@;
}
QRadioButton::indicator:checked:hover {
    border-color: %19;
}
QCheckBox::indicator {
    width: %15px;
    height: %15px;
    border-radius: %20px;
    border: 1px solid @checkEdge@;
    background: @checkFill@;
}
QCheckBox::indicator:checked {
    background: @checkFillChecked@;
    border-color: %18;
}
QPushButton {
    color: @buttonText@;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 @buttonTop@, stop:1 @buttonBottom@);
    border: 1px solid %17;
    border-radius: %7px;
    padding: %21px %22px;
    font-weight: 600;
}
QPushButton:hover {
    border-color: @buttonHoverEdge@;
    color: %23;
}
/* A checkable QPushButton had no checked state in this sheet at all -- only
   the tab variant below did -- so BYPASS, Record and Play rendered identically
   latched and unlatched, and the operator could not tell whether the voice
   chain was bypassed. This is the same amber the gate's Mode pair and the
   compressor's makeup handle use for "this is on and doing something".
   Tokens, not literals: every caller of this sheet routes it through
   ThemeManager::applyStyleSheet(), which substitutes them. The tab rule
   further down is an attribute selector and therefore more specific, so tabs
   are unaffected.
   Placed ABOVE :disabled deliberately: the two tie on specificity (one
   pseudo-class each on the same type), so the later rule wins, and a button
   that is both checked and disabled must read as disabled rather than as "on
   and doing something". No live state reaches that combination today, but
   this is shared chrome and the next checkable button added under it may
   not be so lucky. */
QPushButton:checked {
    color: {{color.meter.gainReduction}};
    background: {{color.background.tx}};
    border: 1px solid {{color.meter.gainReduction}};
}
QPushButton:disabled {
    color: %11;
    border-color: @disabledEdge@;
    background: @disabledFill@;
}
QPushButton[chrome="tab"] {
    border-radius: %24px;
    border: 1px solid transparent;
    background: transparent;
    min-height: %25px;
    padding: %26px %27px;
    font-size: %28px;
    font-weight: 400;@tabTextRule@
}
QPushButton[chrome="tab"]:checked {
    color: @tabCheckedText@;
    border-color: %18;
    background: @tabCheckedFill@;
}
QPushButton[chrome="tab"]:disabled {
    color: @tabDisabledText@;
}
QPushButton#IconButton {
    min-width: %29px;
    max-width: %29px;
    min-height: %29px;
    max-height: %29px;
    padding: 0px;
}
QComboBox {
    color: %1;
    background: @inputFill@;
    border: 1px solid %17;
    border-radius: %24px;
    padding: %30px %31px;
}
QSpinBox {
    color: %32;
    background: @inputFill@;
    border: 1px solid %17;
    border-radius: %24px;
    padding: %30px %31px;
}
QLineEdit {
    color: %32;
    background: %33;
    border: 1px solid %17;
    border-radius: %7px;
    padding: %30px %31px;
    selection-background-color: @selection@;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: %34px;
}
QLineEdit:focus {
    border-color: %18;
}
QSlider::groove:horizontal {
    background: %33;
    border: 1px solid %17;
    height: %35px;
    border-radius: %36px;
}
QSlider::sub-page:horizontal {
    background: %18;
    border-radius: %36px;
}
QSlider::handle:horizontal {
    background: %19;
    border: 1px solid %2;
    width: %37px;
    height: %37px;
    margin: -%38px 0;
    border-radius: %39px;
}
QSlider::handle:horizontal:disabled {
    background: @sliderHandleDisabled@;
}
QSlider::sub-page:horizontal:disabled {
    background: @sliderFillDisabled@;
}
QTableWidget {
    color: @tableText@;
    background: %33;
    alternate-background-color: @tableAlt@;
    border: none;
    gridline-color: @tableGrid@;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: %34px;
    selection-background-color: @selection@;
}
QTableWidget::item {
    padding: 2px 10px;
}
QHeaderView::section {
    color: %9;
    background: @headerFill@;
    border: none;
    border-bottom: 1px solid %6;
    padding: 5px 8px;
    font-size: %10px;
    font-weight: 700;
}
QProgressBar {
    background: %33;
    border: 1px solid %17;
    border-radius: %36px;
    height: %35px;
    text-align: center;
    color: %9;
    font-size: %10px;
}
QProgressBar::chunk {
    background: %18;
    border-radius: %36px;
}
QScrollBar:vertical {
    background: %2;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: @scrollHandle@;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
)")
        .arg(QLatin1String(c.text))               // 1
        .arg(QLatin1String(c.ground))             // 2
        .arg(baseFont)                             // 3
        .arg(QLatin1String(c.panelTop))           // 4
        .arg(QLatin1String(c.panelBottom))        // 5
        .arg(QLatin1String(c.border))             // 6
        .arg(radius)                               // 7
        .arg(QLatin1String(c.borderSoft))         // 8
        .arg(QLatin1String(c.section))            // 9
        .arg(sectionFont)                          // 10
        .arg(QLatin1String(c.textDim))            // 11
        .arg(QLatin1String(c.statusValue))        // 12
        .arg(QLatin1String(c.statusDot))          // 13
        .arg(spacing)                              // 14
        .arg(indicator)                            // 15
        .arg(indicatorRad)                         // 16
        .arg(QLatin1String(c.controlBorder))      // 17
        .arg(QLatin1String(c.accent))             // 18
        .arg(QLatin1String(c.accentBright))       // 19
        .arg(checkRadius)                          // 20
        .arg(buttonPadV)                           // 21
        .arg(buttonPadH)                           // 22
        .arg(QLatin1String(c.textBright))         // 23
        .arg(radius - 2)                           // 24
        .arg(compact ? 16 : 20)                    // 25
        .arg(tabPadV)                              // 26
        .arg(tabPadH)                              // 27
        .arg(tabFont)                              // 28
        .arg(iconButton)                           // 29
        .arg(fieldPadV)                            // 30
        .arg(fieldPadH)                            // 31
        .arg(QLatin1String(c.fieldText))          // 32
        .arg(QLatin1String(c.field))              // 33
        .arg(fieldFont)                            // 34
        .arg(grooveHeight)                         // 35
        .arg(grooveHeight / 2)                     // 36
        .arg(handleSize)                           // 37
        .arg((handleSize - grooveHeight) / 2 + 1)  // 38
        .arg(handleSize / 2);                      // 39
    for (const auto& [slot, value] : c.literals) {
        sheet.replace(QLatin1String(slot), QLatin1String(value));
    }
    // The canon look's check boxes and radio buttons are the painted canon
    // indicators; their rules are more specific than the ones above.
    if (look == Look::Canon) {
        sheet += canonIndicatorRules();
    }
    return sheet;
}


QColor colour(const char* placeholder, const QWidget* scope)
{
    QString token = QString::fromLatin1(placeholder);
    if (token.startsWith(QLatin1String("{{")) && token.endsWith(QLatin1String("}}"))) {
        token = token.mid(2, token.size() - 4);
    }
    return AetherSDR::ThemeManager::instance().color(scope, token);
}

QColor colour(const char* placeholder)
{
    QString token = QString::fromLatin1(placeholder);
    if (token.startsWith(QLatin1String("{{")) && token.endsWith(QLatin1String("}}"))) {
        token = token.mid(2, token.size() - 4);
    }
    return AetherSDR::ThemeManager::instance().color(token);
}

} // namespace AetherSDR::ModemChrome
