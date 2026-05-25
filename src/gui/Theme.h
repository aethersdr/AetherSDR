#pragma once

// Application-wide base stylesheet template (RFC #3076 Phase 2).
//
// Applied to MainWindow and every top-level floating window so pop-out
// panels inherit the complete theme instead of falling back to the
// system palette.  Returned as a {{token.name}} template — caller wraps
// it in ThemeManager::applyStyleSheet(widget, ...) so the widget gets
// free live re-theme on theme changes.
//
// The 4 call sites are MainWindow, PanFloatingWindow,
// FloatingContainerWindow, and the applet float window; each owns its
// own QWidget-derived top-level container.

#include "core/ThemeManager.h"

#include <QString>
#include <QWidget>

namespace AetherSDR {

inline QString appStylesheetTemplate()
{
    // Token map (post-canonicalisation per docs/theming/canonical-tokens.md):
    //   #0f0f1a / #111120 / #0a0a14   →  color.background.0
    //   #1a2a3a / #161626             →  color.background.1
    //   #203040                       →  color.background.1 (when used as bg)
    //                                    color.border.strong (when used as border)
    //   #c8d8e8                       →  color.text.primary
    //   #00b4d8                       →  color.accent
    //   #000 (text on accent)         →  hardcoded for now; follow-up to add
    //                                    color.text.onAccent for proper contrast
    //                                    tuning in the Phase 4 Light theme.
    //
    // Font: 13px hardcoded; Phase 2 follow-up should canonicalise font sizes.
    return QStringLiteral(R"(
        QWidget {
            background-color: {{color.background.0}};
            color: {{color.text.primary}};
            font-family: "{{font.family.ui}}", "Segoe UI", sans-serif;
            font-size: 13px;
        }
        QGroupBox {
            border: 1px solid {{color.border.strong}};
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            color: {{color.accent}};
        }
        QPushButton {
            background-color: {{color.background.1}};
            border: 1px solid {{color.border.strong}};
            border-radius: 4px;
            padding: 4px 10px;
            color: {{color.text.primary}};
        }
        QPushButton:hover  { background-color: {{color.background.2}}; }
        QPushButton:pressed { background-color: {{color.accent}}; color: #000; }
        QComboBox {
            background-color: {{color.background.1}};
            border: 1px solid {{color.border.strong}};
            border-radius: 4px;
            padding: 3px 6px;
        }
        QComboBox::drop-down { border: none; }
        QListWidget {
            background-color: {{color.background.0}};
            border: 1px solid {{color.border.strong}};
            alternate-background-color: {{color.background.1}};
        }
        QListWidget::item:selected { background-color: {{color.accent}}; color: #000; }
        QSlider::groove:horizontal {
            height: 4px;
            background: {{color.border.strong}};
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px; height: 14px;
            margin: -5px 0;
            background: {{color.accent}};
            border-radius: 7px;
        }
        QMenuBar { background-color: {{color.background.0}}; }
        QMenuBar::item:selected { background-color: {{color.background.1}}; }
        QMenu { background-color: {{color.background.0}}; border: 1px solid {{color.border.strong}}; }
        QMenu::item:selected { background-color: {{color.accent}}; color: #000; }
        QStatusBar { background-color: {{color.background.0}}; border-top: 1px solid {{color.border.strong}}; }
        QProgressBar {
            background-color: {{color.background.0}};
            border: 1px solid {{color.border.strong}};
            border-radius: 3px;
        }
        QSplitter::handle { background-color: {{color.border.strong}}; width: 2px; }
    )");
}

// Apply the themed app-wide stylesheet to `widget` and register it for
// free live-reload on theme change.  Use this instead of
// widget->setStyleSheet(...) at every top-level QMainWindow / floating
// window so the whole tree re-themes simultaneously when the user
// switches themes.
inline void applyAppTheme(QWidget* widget)
{
    if (!widget) return;
    ThemeManager::instance().applyStyleSheet(widget, appStylesheetTemplate());
}

// Back-compat shim — kept so legacy call sites keep compiling during
// the rolling migration.  Returns the resolved stylesheet directly,
// bypassing the widget reverse-map.  Prefer applyAppTheme() instead.
[[deprecated("Use applyAppTheme(widget) for free live re-theme registration")]]
inline QString darkThemeStylesheet()
{
    return ThemeManager::instance().resolve(appStylesheetTemplate());
}

} // namespace AetherSDR
