#pragma once

#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QColor>
#include <QFont>
#include <QBrush>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QVariant>
#include <QVector>

#include <functional>

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
    // stylesheet template OR declared explicitly via declareWidgetTokens().
    // Empty list if the widget was never tracked.
    QStringList tokensForWidget(const QWidget* widget) const;

    // Custom-paint widgets (panadapter, waterfall, meters, slice indicators)
    // read tokens directly inside paintEvent rather than going through a
    // stylesheet template, so applyStyleSheet's reverse-map never sees them.
    // declareWidgetTokens() lets such widgets advertise the tokens they
    // paint with, so the Phase 5 inspector can answer "what paints this?"
    // for paint-code regions too.  Re-call to update; entries are cleared
    // automatically when the widget is destroyed.  Paint-code widgets are
    // not auto-repainted on themeChanged — they're expected to connect
    // themselves to themeChanged and call update().
    void declareWidgetTokens(QWidget* widget, const QStringList& tokens);

    // Sub-region-aware inspector lookup for custom-paint widgets.  Each
    // ThemeRegion ties a token to a hit-test function evaluated in the
    // widget's local coordinate system.  Inspector clicks call
    // tokensAtPoint() to narrow the broad declareWidgetTokens() list down
    // to just the tokens painting the clicked sub-region.
    //
    // Example — a panadapter with separate trace + waterfall areas:
    //   tm.declareWidgetRegions(spectrum, {
    //     { "color.spectrum.trace",      [this](QPoint p){ return panRect().contains(p); }, "FFT trace" },
    //     { "color.waterfall.colormap",  [this](QPoint p){ return wfRect().contains(p);  }, "Waterfall" },
    //   });
    //
    // Multiple regions may match a single point — caller receives all
    // matches in declaration order so the editor can disambiguate.
    struct ThemeRegion {
        QString  token;
        std::function<bool(QPoint localPos)> hitTest;
        QString  description;  // optional; shown alongside the token name
    };
    void declareWidgetRegions(QWidget* widget, const QList<ThemeRegion>& regions);

    // Returns the tokens whose ThemeRegion::hitTest() matches at `localPos`
    // for the widget.  Falls back to tokensForWidget() if the widget has
    // no declared regions (or no region matches the point) — guarantees
    // the inspector always has something to surface for a tracked widget.
    QStringList tokensAtPoint(const QWidget* widget, const QPoint& localPos) const;

    // Stateless helper exposing the same token-extraction regex used
    // by applyStyleSheet().  Tooling (audit scripts, the Phase 5
    // editor's inspector preview) can call this to list every token
    // a template references without actually applying the stylesheet.
    static QStringList extractReferencedTokens(const QString& stylesheetTemplate);

    // Theme management.
    QStringList availableThemes() const;        // built-in + user-dir themes
    QString     activeTheme() const;
    bool        setActiveTheme(const QString& name);

    // Phase 5 — editor support.  Enumerate every token and mutate
    // scalar values in-memory.  Mutations emit themeChanged so every
    // widget registered through applyStyleSheet re-paints with the new
    // value on the next event-loop turn.  Edits are session-local
    // until saved through saveCurrentThemeAs() (writes m_tokens to
    // `~/.config/AetherSDR/themes/<name>.json`).
    QStringList allTokenKeys() const;
    void        setColor(const QString& token, const QColor& color);
    void        setSizing(const QString& token, int value);

    // Structured-gradient accessor + mutator for the Phase 5 gradient
    // editor.  gradient() returns an empty ThemeGradient (zero stops)
    // when the token isn't a gradient — callers should check stops.size()
    // before treating the result as live data.  setGradient() emits
    // themeChanged() so widgets re-paint with the new colormap on the
    // next event-loop turn.
    ThemeGradient gradient(const QString& token) const;
    void          setGradient(const QString& token, const ThemeGradient& g);

    // Factory-default lookup — reads from the bundled
    // `:/themes/default-dark.json` so the gradient editor's
    // "Reset to default" button can restore the canonical colormap
    // shape after the operator wanders into territory they don't like.
    // Returns an empty gradient if the token isn't a gradient in the
    // bundled defaults (caller should check stops.size()).
    ThemeGradient factoryGradient(const QString& token) const;

    bool        saveCurrentThemeAs(const QString& newThemeName);

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

    // Factory-default snapshot, loaded once from `:/themes/default-dark.json`
    // at construction.  Drives the gradient editor's "Reset to default"
    // button.  Lazy-initialised so a totally missing resource bundle
    // doesn't take the whole singleton down.
    mutable QHash<QString, QVariant> m_factoryTokens;
    mutable bool m_factoryLoaded{false};
    void ensureFactoryLoaded() const;

    // Reverse-map: widget instance → (template, tokens-it-references).
    // Populated by applyStyleSheet / declareWidgetTokens / declareWidgetRegions,
    // drained by onTrackedWidgetDestroyed.
    struct TrackedWidget {
        QString             stylesheetTemplate;
        QStringList         tokens;
        QList<ThemeRegion>  regions;
    };
    QHash<QWidget*, TrackedWidget> m_trackedWidgets;
};

// Convenience helper for paint code that needs a themed colour with a
// specific alpha (translucent overlays, glow effects, alpha-modulated
// level meter fills).  Returns ThemeManager::color(token) with the
// alpha channel overridden.
//
// Used by tools/migrate_paint_colours.py output — it emits
// `theme::withAlpha("token", N)` for 4-arg `QColor(R, G, B, A)`
// literals so the resolved colour stays alpha-correct after the
// migration.
namespace theme {
inline QColor withAlpha(const QString& token, int alpha)
{
    QColor c = ThemeManager::instance().color(token);
    c.setAlpha(alpha);
    return c;
}
} // namespace theme

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ThemeGradient)
