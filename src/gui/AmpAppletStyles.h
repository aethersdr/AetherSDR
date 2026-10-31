#pragma once

#include <QString>

namespace AetherSDR {

// Shared visual vocabulary for the amplifier applet family (AcomApplet,
// SpeApplet — and the next serial amp when it comes). These were per-applet
// copies until the colour ratchet (#4569) made the duplication visible:
// keeping the strings in one place means the family stays visually
// identical by construction, and hex literals are counted once.
//
// Styles are templates for ThemeManager::applyStyleSheet ({{color.*}}
// tokens re-resolve on theme change); the pill/active hexes are the
// family's established values, predating the token set.

// Status-pill appearance: transmitting / receiving / standby / anything
// else (off, unknown, not responding).
enum class AmpPillState { OperateTx, OperateRx, Standby, Neutral };

inline QString ampPillStyle(AmpPillState state)
{
    switch (state) {
        case AmpPillState::OperateTx:
            return QStringLiteral(
                "QLabel { background: #3a1418; color: #ff8080; border: 1px solid #ff4d4d; "
                "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }");
        case AmpPillState::OperateRx:
            return QStringLiteral(
                "QLabel { background: #0f2a1c; color: #6be899; border: 1px solid #4dd87a; "
                "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }");
        case AmpPillState::Standby:
            return QStringLiteral(
                "QLabel { background: #12222e; color: #7fc4dc; border: 1px solid #2a5a70; "
                "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }");
        default:
            return QStringLiteral(
                "QLabel { background: #1c222a; color: #6b7684; border: 1px solid #303a44; "
                "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }");
    }
}

// A button rendered in an "active state" colour pair (background, border).
inline QString ampActiveBtnStyle(const QString& bg, const QString& border)
{
    return QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; border-radius: 3px; "
        "color: {{color.text.primary}}; font-size: 10px; font-weight: bold; } "
        "QPushButton:hover { background: %1; }").arg(bg, border);
}

// The family's operate-active green (OPERATE engaged, power-on affordance).
inline QString ampOperateActiveBtnStyle()
{
    return ampActiveBtnStyle(QStringLiteral("#006030"), QStringLiteral("#008040"));
}

inline QString ampNeutralBtnStyle()
{
    return QStringLiteral(
        "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }");
}

// Standby-state key — amber via theme tokens, matching AmpApplet's
// kPanelKeyStandbyStyle. Used when the amp is connected, responding,
// and in standby (not operate).
inline QString ampStandbyBtnStyle()
{
    return QStringLiteral(
        "QPushButton { background: {{color.accessory.key.standby.background}}; "
        "border: 1px solid {{color.accessory.key.standby.foreground}}; border-radius: 3px; "
        "color: {{color.accessory.key.standby.foreground}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }");
}

// Compact CLEAR FAULT button — amber-on-dark, sized to sit in-grid rather
// than the full button row. Used by AcomApplet and Kpa500Applet.
inline QString ampClearFaultBtnStyle()
{
    return QStringLiteral(
        "QPushButton { background: #2a2210; border: 1px solid #5a4a1a; "
        "border-radius: 3px; color: #ffb84d; font-size: 9px; font-weight: bold; padding: 1px 4px; }"
        "QPushButton:hover { background: #3a2e14; }"
        "QPushButton:disabled { background: #181c22; border: 1px solid #232a33; color: #3a4552; }");
}

}  // namespace AetherSDR
