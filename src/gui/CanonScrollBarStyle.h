#pragma once

// The canon scroll bar (style guide, RFC #6226): a 12 px transparent track,
// a color.canon.lineHi handle with rounded ends, and no step arrows, both
// orientations. Template text for ThemeManager::applyStyleSheet() or
// applyCanonSheet(); a canon window appends it to its sheet.

#include <QString>

namespace AetherSDR {

inline QString canonScrollBarRules()
{
    return QStringLiteral(R"(
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 12px;
    margin: 2px 8px 2px 8px;
    border-radius: 6px;
}
QScrollBar::handle:horizontal {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-width: 34px;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0px;
}
)");
}

} // namespace AetherSDR
