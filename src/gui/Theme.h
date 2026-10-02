#pragma once

// App-wide base stylesheet template (RFC #3076), applied via applyAppTheme() to
// MainWindow and every top-level floating window so pop-outs inherit the full
// theme instead of the system palette, with live re-theme.

#include "core/ThemeManager.h"

#include <QEvent>
#include <QObject>
#include <QString>
#include <QWidget>

namespace AetherSDR {

namespace detail {

// Swallows slider HoverEnter/Move/Leave so QSliderPrivate::updateHoverControl()
// never repaints (#4869). Filters events rather than clearing WA_Hover because
// QFusionStyle::polish() sets WA_Hover on every QAbstractSlider by type, including
// via direct style()->polish() calls that emit no Polish/StyleChange event.
// One process-wide instance; installEventFilter() de-duplicates registrations.
class SliderHoverSuppressor : public QObject {
public:
    static SliderHoverSuppressor& instance()
    {
        static SliderHoverSuppressor s;
        return s;
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::HoverLeave:
            return true;
        default:
            return false;
        }
    }
};

} // namespace detail

inline QString appStylesheetTemplate()
{
    // Tokens per docs/theming/canonical-tokens.md. Text on accent (#000) and the
    // 13px font size are still literals.
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

// Canonical primary slider style (sub-page fill + plain handle, no hover).
// `accentToken` sets the fill; default `color.slider.foreground` (separate from
// `color.accent` so retinting sliders doesn't touch buttons). Per-slice or
// TX-amber callers pass their own token. Don't add a :hover rule without first
// removing the hover-event suppression in applyPrimarySliderStyle() (#4869).
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

// Apply the primary slider style with live re-theme; `accentToken` as above,
// resolved widget-aware so applet scope overrides apply. Also suppresses hover
// repaints (#4869): Fusion sets WA_Hover on every slider, and at a fractional
// DPR (e.g. 125%/150% UI scale) the no-op hover repaint leaves stale pixels.
// Consequence: a :hover rule in primarySliderStyleTemplate() would never render.
inline void applyPrimarySliderStyle(QWidget* slider,
                                    const QString& accentToken = QStringLiteral("color.slider.foreground"))
{
    if (!slider) return;
    ThemeManager::instance().applyStyleSheet(slider, primarySliderStyleTemplate(accentToken));
    // One shared filter instance serves every themed slider (and, via
    // GuardedSlider's own constructor, every GuardedSlider regardless of
    // styling path — see GuardedSlider.h). installEventFilter() de-dups
    // internally, so installing it twice on the same widget is harmless.
    slider->installEventFilter(&detail::SliderHoverSuppressor::instance());
}

// Toggle button tribes: each resolves its checked {background, foreground,
// border} from `color.toggle.<tribe>.*`; Accent also follows per-applet scope
// overrides. Choose by what the checked state means:
//   - Accent  - generic on/off, mode selectors
//   - Success - enable / activate / connect (green)
//   - Warning - caution / armed / high-stakes (amber)
enum class ToggleTribe { Accent, Success, Warning };

inline QLatin1String toggleTribePrefix(ToggleTribe tribe)
{
    switch (tribe) {
        case ToggleTribe::Success: return QLatin1String("color.toggle.success");
        case ToggleTribe::Warning: return QLatin1String("color.toggle.warning");
        case ToggleTribe::Accent:
        default:                   return QLatin1String("color.toggle.accent");
    }
}

// Canonical checkable QPushButton style. `tribe` picks the checked-state colour
// family; unchecked and disabled come from the shared `color.toggle.*` tokens.
// Resolution is widget-aware, so the Accent tribe follows per-applet overrides
// (TxApplet red, RxApplet green, ClientCompApplet amber).
inline QString primaryToggleButtonStyleTemplate(ToggleTribe tribe = ToggleTribe::Accent)
{
    const QString prefix(toggleTribePrefix(tribe));
    return QStringLiteral(
        "QPushButton {"
        " background: {{color.toggle.background}};"
        " color: {{color.toggle.foreground}};"
        " border: 1px solid {{color.toggle.border}};"
        " border-radius: 3px;"
        " padding: 2px 8px;"
        " font-size: 11px;"
        " font-weight: bold;"
        " }"
        "QPushButton:hover { background: {{color.background.2}}; }"
        "QPushButton:checked {"
        " background: {{%1.background.checked}};"
        " color: {{%1.foreground.checked}};"
        " border: 1px solid {{%1.border.checked}};"
        " }"
        "QPushButton:disabled {"
        " background: {{color.toggle.background.disabled}};"
        " color: {{color.toggle.foreground.disabled}};"
        " border: 1px solid {{color.toggle.border.disabled}};"
        " }"
    ).arg(prefix);
}

// Apply the canonical primary toggle-button style to `btn` and register
// it for free live re-theme on theme changes.  Use this in place of
// per-site `applyStyleSheet(btn, kSomeInlineToggleStyle)` so the toggle
// inherits the namespace + per-applet cascade automatically.
inline void applyToggleButtonStyle(QWidget* btn, ToggleTribe tribe = ToggleTribe::Accent)
{
    if (!btn) return;
    ThemeManager::instance().applyStyleSheet(btn, primaryToggleButtonStyleTemplate(tribe));
}

} // namespace AetherSDR
