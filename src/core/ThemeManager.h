#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QColor>
#include <QFont>
#include <QBrush>
#include <QPointF>
#include <QRect>
#include <QVariant>
#include <QVector>

class QWidget;

namespace AetherSDR {

// Token-based theming subsystem (RFC #3076 Phase 1+2).
//
// Every visual decision in the GUI — colours, fonts, key spacings —
// resolves through a named token (e.g. "color.accent", "font.size.normal").
// Themes are JSON files at ~/.config/AetherSDR/themes/<name>.json plus the
// built-in default-dark / default-light shipped under :/themes/.
//
// Phase 1 shipped: manager singleton, scalar token API, JSON loader,
// stylesheet template resolver, ActiveTheme persistence, default-dark.json
// baked into resources.
//
// Phase 2 adds (this commit): first-class gradient tokens.  A color token
// can be a scalar (#rrggbb) or a gradient object describing a linear or
// radial gradient with N stops.  brush() returns a QBrush wrapping the
// resolved Qt gradient; cssFragment() emits the matching qlineargradient
// / qradialgradient stylesheet syntax; resolve() routes through
// cssFragment() so existing {{token}} templating "just works" for
// gradient-typed tokens.

// Gradient definition stored inside m_tokens.  Lives in the public header
// so the audit/editor tooling can inspect / mutate themes by value.
struct ThemeGradientStop {
    qreal  at    = 0.0;   // 0.0–1.0 position
    QColor color;
};

struct ThemeGradient {
    enum Type { Linear, Radial };

    Type    type   = Linear;
    // Linear: CSS-convention angle.  0deg = bottom→top, 90deg = left→right,
    // 180deg = top→bottom, 270deg = right→left.  Mirrors the CSS3
    // linear-gradient() syntax so designers can pull values straight from
    // CSS or DevTools.
    qreal   angle  = 180.0;
    // Radial: normalised centre + radius in 0–1 units of the painted area.
    QPointF center{0.5, 0.5};
    qreal   radius = 0.5;
    QVector<ThemeGradientStop> stops;
};

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    // Scalar accessors.  Missing tokens log a warning and return the
    // compiled-in default for the type.  For gradient-typed tokens,
    // color() returns the gradient's first stop as a graceful fallback;
    // callers that want the full gradient should use brush() or
    // cssFragment().
    QColor   color(const QString& token) const;
    QFont    font(const QString& token) const;
    int      sizing(const QString& token) const;
    QString  value(const QString& token) const;   // raw scalar value, "" for gradients

    // Brush accessor — returns the right Qt brush type for the token.
    //   - scalar token  → QBrush(QColor)
    //   - linear        → QBrush(QLinearGradient) mapped onto `bounds`
    //   - radial        → QBrush(QRadialGradient) mapped onto `bounds`
    // `bounds` only matters for gradient tokens; pass the widget rect or
    // the paint area when drawing into a specific QPainter.  An empty
    // QRect produces a 0–1 normalised gradient suitable for stylesheets
    // that reference the brush via QPalette or Qt's stylesheet system.
    QBrush   brush(const QString& token, const QRect& bounds = QRect()) const;

    // Stylesheet fragment.  Emits the right syntax for use inside a Qt
    // stylesheet:
    //   - scalar token  → "#rrggbb"
    //   - linear        → "qlineargradient(x1:.., y1:.., x2:.., y2:..,
    //                       stop:0 #aabbcc, stop:1 #ddeeff)"
    //   - radial        → "qradialgradient(cx:.., cy:.., radius:.., fx:.., fy:..,
    //                       stop:0 #aabbcc, stop:1 #ddeeff)"
    // Numeric tokens emit their value as a plain string ("12" — adding
    // "px" / unit suffix is the caller's responsibility).
    QString  cssFragment(const QString& token) const;

    // Stylesheet template resolver.  Replaces every "{{token.name}}"
    // placeholder by calling cssFragment(), so a stylesheet like
    //   "QPushButton { background: {{color.button.idle}}; }"
    // gets a literal "#aabbcc" or a "qlineargradient(...)" inlined
    // depending on whether the token is scalar or gradient.
    QString  resolve(const QString& stylesheetTemplate) const;

    // Apply a stylesheet template to a widget AND record the
    // (widget → tokens referenced) reverse-map.  Phase 5's inspector
    // uses this map to answer "which tokens paint this widget?" when
    // the operator clicks during inspect mode.
    //
    // Additionally: widgets registered through applyStyleSheet get free
    // live theme switching — the manager listens to themeChanged and
    // re-applies the recorded template (with newly resolved values) so
    // stylesheet-painted widgets respond to theme changes without any
    // per-call-site wiring.
    //
    // The recorded entry is removed automatically when the widget is
    // destroyed (via QObject::destroyed signal connection), so no
    // dangling pointers.
    void applyStyleSheet(QWidget* widget, const QString& stylesheetTemplate);

    // Stop tracking a widget — its recorded stylesheet template is
    // dropped and it no longer re-paints on themeChanged.  Useful for
    // widgets that want to take over stylesheet management themselves
    // after an initial themed apply.
    void clearWidgetTracking(QWidget* widget);

    // Inspector lookup: tokens referenced by the widget's last-applied
    // stylesheet template.  Empty list if the widget was never themed
    // through applyStyleSheet().
    QStringList tokensForWidget(const QWidget* widget) const;

    // Stateless helper exposing the same token-extraction regex used
    // by applyStyleSheet().  Tooling (audit scripts, the Phase 5
    // editor's inspector preview) can call this to list every token
    // a template references without actually applying the stylesheet.
    static QStringList extractReferencedTokens(const QString& stylesheetTemplate);

    // Theme management.
    QStringList availableThemes() const;        // built-in + user-dir themes
    QString     activeTheme() const;
    bool        setActiveTheme(const QString& name);

    // Phase 1 didn't implement save / import / export — those land with
    // the editor in Phase 5.  Reserved on the API surface so consumers
    // can be written against the final shape from day 1.

signals:
    // Fired whenever the active theme changes.  Every widget that reads
    // tokens connects here and calls update() / re-applies its stylesheet.
    // Stylesheet-painted widgets registered through applyStyleSheet() are
    // re-themed automatically; paint-code consumers connect themselves.
    void themeChanged();

private slots:
    // Cleanup hook — fired when a widget tracked through applyStyleSheet
    // is destroyed.  Removes its entry from the reverse-map.
    void onTrackedWidgetDestroyed(QObject* obj);

private:
    ThemeManager();
    ~ThemeManager() override = default;
    Q_DISABLE_COPY_MOVE(ThemeManager)

    // Re-apply every tracked widget's stylesheet template with freshly
    // resolved token values.  Wired to themeChanged in the constructor.
    void reapplyAllTrackedStyleSheets();

    // Discover available themes on construction: scan :/themes/ for
    // built-ins, ~/.config/AetherSDR/themes/ for user themes.
    void scanAvailableThemes();

    // Load tokens from a theme file (built-in path or filesystem path)
    // into m_tokens.  Returns true on success; tokens from a failed load
    // are not committed (the previously-active theme stays loaded).
    bool loadThemeFromPath(const QString& path);

    // Built-in compiled-in defaults so a totally missing theme file
    // still produces a usable UI.  Populated in the constructor.
    void seedBuiltinDefaults();

    // Resource path or filesystem path indexed by theme display name.
    QHash<QString, QString> m_themePaths;
    // QVariant holds either a QString (scalar) or a ThemeGradient
    // (typed via Q_DECLARE_METATYPE below).
    QHash<QString, QVariant> m_tokens;
    QString m_activeTheme;

    // Reverse-map: widget instance → (template, tokens-it-references).
    // Populated by applyStyleSheet, drained by onTrackedWidgetDestroyed.
    struct TrackedWidget {
        QString     stylesheetTemplate;
        QStringList tokens;
    };
    QHash<QWidget*, TrackedWidget> m_trackedWidgets;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ThemeGradient)
