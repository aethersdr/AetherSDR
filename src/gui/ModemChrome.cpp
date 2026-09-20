#include "ModemChrome.h"
#include "core/ThemeManager.h"

namespace AetherSDR::ModemChrome {

QString styleSheet(Scale scale)
{
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

    return QStringLiteral(R"(
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
    background: #08111d;
}
QRadioButton::indicator:checked {
    border: 2px solid %18;
    background: #132d26;
}
QRadioButton::indicator:checked:hover {
    border-color: %19;
}
QCheckBox::indicator {
    width: %15px;
    height: %15px;
    border-radius: %20px;
    border: 1px solid #34533c;
    background: #0d1a18;
}
QCheckBox::indicator:checked {
    background: #5ebd69;
    border-color: %18;
}
QPushButton {
    color: %1;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #142235, stop:1 #0b1625);
    border: 1px solid %17;
    border-radius: %7px;
    padding: %21px %22px;
    font-weight: 600;
}
QPushButton:hover {
    border-color: #3c526d;
    color: %23;
}
QPushButton:disabled {
    color: %11;
    border-color: #1d2a3c;
    background: #0b1522;
}
QPushButton[chrome="tab"] {
    border-radius: %24px;
    border: 1px solid transparent;
    background: transparent;
    min-height: %25px;
    padding: %26px %27px;
    font-size: %28px;
    font-weight: 400;
}
QPushButton[chrome="tab"]:checked {
    color: #d4deea;
    border-color: %18;
    background: #0d1c20;
}
QPushButton[chrome="tab"]:disabled {
    color: #7f8b9e;
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
    background: #0b1625;
    border: 1px solid %17;
    border-radius: %24px;
    padding: %30px %31px;
}
QSpinBox {
    color: %32;
    background: #0b1625;
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
    selection-background-color: #1b3650;
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
    background: #2d3a45;
}
QSlider::sub-page:horizontal:disabled {
    background: #22402a;
}
QTableWidget {
    color: #c2ccdb;
    background: %33;
    alternate-background-color: #081220;
    border: none;
    gridline-color: #14202f;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: %34px;
    selection-background-color: #1b3650;
}
QTableWidget::item {
    padding: 2px 10px;
}
QHeaderView::section {
    color: %9;
    background: #0d1825;
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
    background: #25364d;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
)")
        .arg(QLatin1String(Colour::Text))          // 1
        .arg(QLatin1String(Colour::Background))    // 2
        .arg(baseFont)                             // 3
        .arg(QLatin1String(Colour::PanelTop))      // 4
        .arg(QLatin1String(Colour::PanelBottom))   // 5
        .arg(QLatin1String(Colour::Border))        // 6
        .arg(radius)                               // 7
        .arg(QLatin1String(Colour::BorderSoft))    // 8
        .arg(QLatin1String(Colour::Section))       // 9
        .arg(sectionFont)                          // 10
        .arg(QLatin1String(Colour::TextDim))       // 11
        .arg(QLatin1String(Colour::StatusValue))   // 12
        .arg(QLatin1String(Colour::Green))         // 13
        .arg(spacing)                              // 14
        .arg(indicator)                            // 15
        .arg(indicatorRad)                         // 16
        .arg(QLatin1String(Colour::ControlBorder)) // 17
        .arg(QLatin1String(Colour::GreenEdge))     // 18
        .arg(QLatin1String(Colour::GreenBright))   // 19
        .arg(checkRadius)                          // 20
        .arg(buttonPadV)                           // 21
        .arg(buttonPadH)                           // 22
        .arg(QLatin1String(Colour::TextBright))    // 23
        .arg(radius - 2)                           // 24
        .arg(compact ? 16 : 20)                    // 25
        .arg(tabPadV)                              // 26
        .arg(tabPadH)                              // 27
        .arg(tabFont)                              // 28
        .arg(iconButton)                           // 29
        .arg(fieldPadV)                            // 30
        .arg(fieldPadH)                            // 31
        .arg(QLatin1String(Colour::FieldText))     // 32
        .arg(QLatin1String(Colour::Field))         // 33
        .arg(fieldFont)                            // 34
        .arg(grooveHeight)                         // 35
        .arg(grooveHeight / 2)                     // 36
        .arg(handleSize)                           // 37
        .arg((handleSize - grooveHeight) / 2 + 1)  // 38
        .arg(handleSize / 2);                      // 39
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
