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
        QSlider {
            border: none;
            background: transparent;
        }
        /* Slider components now read from the dedicated color.slider.*
           namespace (carved out of color.background.1 / color.accent /
           color.text.primary so designers can retint sliders without
           rippling into buttons / borders / body text).  Vertical
           rules added alongside horizontal so the canonical look
           matches regardless of orientation. */
        QSlider::groove:horizontal {
            height: 4px;
            background: {{color.slider.background}};
            border: none;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: {{color.slider.foreground}};
            border: none;
            border-radius: 2px;
        }
        QSlider::add-page:horizontal {
            background: {{color.slider.background}};
            border: none;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 12px; height: 12px;
            margin: -4px 0;
            background: {{color.slider.handle}};
            border: none;
            border-radius: 6px;
        }
        QSlider::groove:vertical {
            width: 4px;
            background: {{color.slider.background}};
            border: none;
            border-radius: 2px;
        }
        QSlider::sub-page:vertical {
            background: {{color.slider.foreground}};
            border: none;
            border-radius: 2px;
        }
        QSlider::add-page:vertical {
            background: {{color.slider.background}};
            border: none;
            border-radius: 2px;
        }
        QSlider::handle:vertical {
            width: 12px; height: 12px;
            margin: 0 -4px;
            background: {{color.slider.handle}};
            border: none;
            border-radius: 6px;
        }
        /* Disabled state — quieter background, washed-out fill,
           dim handle.  Hover / pressed states still fall through
           to Qt defaults (deferred per design review). */
        QSlider::groove:horizontal:disabled,
        QSlider::groove:vertical:disabled,
        QSlider::add-page:horizontal:disabled,
        QSlider::add-page:vertical:disabled {
            background: {{color.slider.background.disabled}};
        }
        QSlider::sub-page:horizontal:disabled,
        QSlider::sub-page:vertical:disabled {
            background: {{color.slider.foreground.disabled}};
        }
        QSlider::handle:horizontal:disabled,
        QSlider::handle:vertical:disabled {
            background: {{color.slider.handle.disabled}};
        }
        QMenuBar { background-color: {{color.background.0}}; }
        QMenuBar::item:selected { background-color: {{color.background.1}}; }
        QMenu { background-color: {{color.background.0}}; border: 1px solid {{color.border.strong}}; }
        QMenu::item:selected { background-color: {{color.accent}}; color: #000; }
        QMenu::separator { height: 1px; background: {{color.border.strong}}; margin: 4px 8px; }
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

// Canonical primary slider style — sub-page fill (Wave-style value
// indicator) + plain light handle (Vfo-style, no border, no hover).
// Pre-consolidation the codebase had eight near-duplicate stylesheet
// constants; this helper is the single source of truth.
//
// The `accentToken` parameter controls the sub-page fill colour.
// Default is `color.slider.foreground` (the canonical slider fill —
// carved out of `color.accent` so retinting sliders no longer ripples
// into buttons / borders / focus rings).  Call sites that need a per-
// slice colour (slicePrimarySliderStyle) or a TX-warning amber pass
// their own token here — those overrides still work since the helper
// just substitutes whatever token name is provided.
inline QString primarySliderStyleTemplate(const QString& accentToken = QStringLiteral("color.slider.foreground"))
{
    return QStringLiteral(
        "QSlider { border: none; background: transparent; }"
        "QSlider::groove:horizontal { height: 4px; background: {{color.slider.background}}; border: none; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: {{%1}}; border: none; border-radius: 2px; }"
        "QSlider::add-page:horizontal { background: {{color.slider.background}}; border: none; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0;"
        " background: {{color.slider.handle}}; border: none; border-radius: 6px; }"
        "QSlider::groove:vertical { width: 4px; background: {{color.slider.background}}; border: none; border-radius: 2px; }"
        "QSlider::sub-page:vertical { background: {{%1}}; border: none; border-radius: 2px; }"
        "QSlider::add-page:vertical { background: {{color.slider.background}}; border: none; border-radius: 2px; }"
        "QSlider::handle:vertical { width: 12px; height: 12px; margin: 0 -4px;"
        " background: {{color.slider.handle}}; border: none; border-radius: 6px; }"
        "QSlider::groove:horizontal:disabled, QSlider::groove:vertical:disabled,"
        " QSlider::add-page:horizontal:disabled, QSlider::add-page:vertical:disabled"
        " { background: {{color.slider.background.disabled}}; }"
        "QSlider::sub-page:horizontal:disabled, QSlider::sub-page:vertical:disabled"
        " { background: {{color.slider.foreground.disabled}}; }"
        "QSlider::handle:horizontal:disabled, QSlider::handle:vertical:disabled"
        " { background: {{color.slider.handle.disabled}}; }"
    ).arg(accentToken);
}

// Apply the canonical primary slider style to `slider` and register it
// for free live re-theme on theme changes.  Use this in place of every
// previous `slider->setStyleSheet(kSliderStyle)` call.
//
// Default `accentToken` is `color.slider.foreground` — the canonical
// slider fill token, carved out of `color.accent`.  Callers that
// hard-code a specific token (e.g. `color.accent.warning` for
// TX-adjacent panels, or `color.slice.a` for per-slice colour) still
// work — that token name is what gets substituted.  Resolution is
// widget-aware (walks the slider's container chain), so the applet's
// scope override naturally reaches the rendered output without any
// per-call-site change.
inline void applyPrimarySliderStyle(QWidget* slider,
                                    const QString& accentToken = QStringLiteral("color.slider.foreground"))
{
    if (!slider) return;
    ThemeManager::instance().applyStyleSheet(slider, primarySliderStyleTemplate(accentToken));
}

} // namespace AetherSDR
