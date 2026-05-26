#pragma once

#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QColor>
#include <QFont>
#include <QBrush>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QVariant>
#include <QVector>

#include <QJsonObject>

#include <functional>
#include <map>
#include <memory>

class QWidget;

namespace AetherSDR {

// Forward declaration — full definition lives in the .cpp.  The scope
// tree is an implementation detail of ThemeManager but the public
// scope-aware API needs to reference it indirectly via QString paths.
struct ThemeScope;

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

// Compound font token — bundles the typeface family, point size, and a
// recommended foreground color for a typographic role.  Used by the
// `font.family.*` token namespace (which historically stored a bare
// family string).  JSON shape: { "family": "Inter", "size": 12,
// "color": "#c8d8e8" }; the `family` field is required, `size` defaults
// to 0 (caller uses its role-default), `color` defaults to invalid
// (caller falls back to color.text.primary).
//
// Backward compat: `ThemeManager::value(token)` returns the .family
// field when called on a ThemeFont so the ~35 sites that just want
// the family name keep working unchanged.
struct ThemeFont {
    QString family;
    int     size  {0};   // 0 = unset
    QColor  color;       // invalid = unset
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
    //
    // Two flavours:
    //   * `color(token)` — root-scope lookup (the historical flat
    //     namespace).  Every existing call site routes through this
    //     overload unchanged.
    //   * `color(widget, token)` — walks the widget's Qt parent chain
    //     looking for a `themeContainer` property; the first such
    //     ancestor's container path is used as the lookup origin.  Walks
    //     the scope tree from there up to root, returning the first
    //     match.  Falls back to root-scope lookup for widgets with no
    //     declared container in their ancestry.
    QColor   color(const QString& token) const;
    QColor   color(const QWidget* widget, const QString& token) const;
    QFont    font(const QString& token) const;
    QFont    font(const QWidget* widget, const QString& token) const;
    int      sizing(const QString& token) const;
    int      sizing(const QWidget* widget, const QString& token) const;
    QString  value(const QString& token) const;   // raw scalar value, "" for gradients
    QString  value(const QWidget* widget, const QString& token) const;

    // Brush accessor — returns the right Qt brush type for the token.
    //   - scalar token  → QBrush(QColor)
    //   - linear        → QBrush(QLinearGradient) mapped onto `bounds`
    //   - radial        → QBrush(QRadialGradient) mapped onto `bounds`
    // `bounds` only matters for gradient tokens; pass the widget rect or
    // the paint area when drawing into a specific QPainter.  An empty
    // QRect produces a 0–1 normalised gradient suitable for stylesheets
    // that reference the brush via QPalette or Qt's stylesheet system.
    QBrush   brush(const QString& token, const QRect& bounds = QRect()) const;
    QBrush   brush(const QWidget* widget, const QString& token,
                   const QRect& bounds = QRect()) const;

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
    QString  cssFragment(const QWidget* widget, const QString& token) const;

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

    // Scope-aware setters.  Bare-token overloads (existing call sites)
    // write to the root scope.  Container-path overloads write to a
    // named scope, creating it (and any missing parents) on demand.
    //   - `setColor("color.accent", c)`                  → root["color.accent"] = c
    //   - `setColor("spectrum", "color.accent", c)`      → root.spectrum["color.accent"] = c
    //   - `setColor("spectrum/panadapter", "...", c)`    → nested two levels deep
    // Path segments are separated by '/'.  Empty path == root.
    void        setColor(const QString& token, const QColor& color);
    void        setColor(const QString& containerPath,
                         const QString& token, const QColor& color);
    void        setSizing(const QString& token, int value);
    void        setSizing(const QString& containerPath,
                          const QString& token, int value);

    // Structured-gradient accessor + mutator for the Phase 5 gradient
    // editor.  gradient() returns an empty ThemeGradient (zero stops)
    // when the token isn't a gradient — callers should check stops.size()
    // before treating the result as live data.  setGradient() emits
    // themeChanged() so widgets re-paint with the new colormap on the
    // next event-loop turn.
    ThemeGradient gradient(const QString& token) const;
    ThemeGradient gradient(const QWidget* widget, const QString& token) const;
    void          setGradient(const QString& token, const ThemeGradient& g);
    void          setGradient(const QString& containerPath,
                              const QString& token, const ThemeGradient& g);

    // Family / font-family setter for the Phase 5 PR 4 font picker.
    // Mirrors setColor()/setSizing() — overwrites whatever was at the
    // token and emits themeChanged so consumers re-resolve their fonts.
    void setString(const QString& token, const QString& value);
    void setString(const QString& containerPath,
                   const QString& token, const QString& value);

    // Compound font-token accessors.  font.family.* tokens may store
    // either a bare family string (legacy v1 themes) or a structured
    // ThemeFont (family + size + color).  These accessors abstract over
    // both shapes — `fontToken*()` returns the ThemeFont with sensible
    // defaults filled in from the legacy string + role-default size/color
    // when the token isn't compound.
    ThemeFont     fontToken(const QString& token) const;
    ThemeFont     fontTokenAt(const QString& containerPath,
                              const QString& token) const;
    void          setFontToken(const QString& token, const ThemeFont& f);
    void          setFontToken(const QString& containerPath,
                               const QString& token, const ThemeFont& f);

    // Container-tree introspection (used by the v2 Theme Editor's
    // container picker — left rail tree + columnar overrides table).
    //   * `containerPaths()` — every scope present in the active theme,
    //     "" for root.  Sorted in tree order so the editor can render
    //     them as a hierarchical tree.
    //   * `containerPathFor(widget)` — walks `widget`'s Qt parent chain
    //     looking for the nearest `themeContainer` property; returns ""
    //     if none of `widget`'s ancestors declared one.
    QStringList containerPaths() const;
    QString     containerPathFor(const QWidget* widget) const;

    // Path-string scope-aware getters.  Mirror the widget-aware
    // overloads but accept a literal container path string — used by
    // the v2 editor whose container picker holds a path, not a widget.
    QColor        colorAt(const QString& containerPath, const QString& token) const;
    int           sizingAt(const QString& containerPath, const QString& token) const;
    QString       valueAt(const QString& containerPath, const QString& token) const;
    ThemeGradient gradientAt(const QString& containerPath, const QString& token) const;
    // Indicates whether `containerPath` itself overrides `token` (true)
    // or just inherits it from an ancestor (false).
    bool          isOverriddenAt(const QString& containerPath, const QString& token) const;

    // Drop the local override for `token` at `containerPath`, falling
    // back to inheritance from the scope's parent.  No-op when there
    // is no override to drop.  Emits themeChanged + persists to disk
    // through saveActiveTheme() so the inheritance restoration
    // survives a restart.
    void          removeOverride(const QString& containerPath, const QString& token);

    // Container declarations — widgets call this through
    // theme::setContainer() to register their scope; ThemeManager keeps
    // the path alive in m_declaredContainers so it stays visible in
    // containerPaths() even after a theme load wipes empty scopes from
    // the tree.  Idempotent.
    void        registerDeclaredContainer(const QString& containerPath);

    // Widget-aware QSS resolver — replaces each {{token}} placeholder
    // with the widget's-scope css fragment instead of the root-only
    // value.  applyStyleSheet() routes through this so widgets nested
    // under a declared container automatically pick up its overrides
    // on the next reapplyAllTrackedStyleSheets() pass.
    QString     resolveFor(const QWidget* widget, const QString& stylesheetTemplate) const;

    // Factory-default lookups — read from a one-shot snapshot of the
    // bundled `:/themes/default-dark.json` so every Reset-to-default
    // affordance in the editor restores the canonical value.  Each
    // returns a sentinel (empty / invalid / 0 / -1) when the token has
    // no factory baseline — callers should check before using.
    ThemeGradient factoryGradient(const QString& token) const;
    QColor        factoryColor(const QString& token) const;
    int           factorySizing(const QString& token) const;     // -1 = none
    QString       factoryString(const QString& token) const;
    bool          hasFactoryValue(const QString& token) const;

    // Theme-file management — Delete / Rename for the user's saved
    // themes living under ~/.config/AetherSDR/themes/.  Both refuse
    // on built-in themes (those live inside the Qt resource bundle
    // and aren't deletable).  Delete switches the active theme back
    // to "Default Dark" before unlinking so the UI doesn't render
    // half-blank during the file removal.
    bool        deleteTheme(const QString& name);
    bool        renameTheme(const QString& oldName, const QString& newName);
    bool        isBuiltInTheme(const QString& name) const;

    bool        saveCurrentThemeAs(const QString& newThemeName);

    // Persist the current in-memory token state back to the active
    // theme's on-disk file.  No-op for built-in themes (their path
    // points at the read-only resource bundle) and for unknown
    // themes.  Called automatically by setColor / setSizing /
    // setGradient / setString so edits survive a restart without
    // requiring an explicit "Save" gesture.
    bool        saveActiveTheme();

    // Phase 6 — share-friendly file format.  `.aethertheme` is plain JSON
    // (same shape `saveCurrentThemeAs` writes to the user-dir) with a
    // `schemaVersion` discriminator.  Missing tokens on import fall back
    // to the built-in defaults; unknown tokens round-trip unchanged so
    // a future v2 theme still loads on this v1 build (just with the
    // unknown tokens unused).
    //
    // exportThemeToFile():
    //   * `themeName` — name registered with ThemeManager.  Pass
    //     `activeTheme()` to dump the live state.
    //   * `filePath`  — absolute target path.  Caller picks the dialog;
    //     this method just writes JSON.
    //
    // importThemeFromFile():
    //   * Validates magic + schema, picks a theme name from the JSON's
    //     "name" field (fallback: file stem), copies the file into
    //     `~/.config/AetherSDR/themes/`, registers it in m_themePaths,
    //     and makes it the active theme.
    //   * Returns the imported theme's display name on success, empty
    //     on failure.  Caller surfaces the failure reason via the
    //     `errorMessage` out-param.
    bool    exportThemeToFile(const QString& themeName,
                              const QString& filePath,
                              QString* errorMessage = nullptr) const;
    QString importThemeFromFile(const QString& filePath,
                                QString* errorMessage = nullptr);

signals:
    // Fired whenever the active theme changes.  Every widget that reads
    // tokens connects here and calls update() / re-applies its stylesheet.
    // Stylesheet-painted widgets registered through applyStyleSheet() are
    // re-themed automatically; paint-code consumers connect themselves.
    void themeChanged();

protected:
    // Re-resolve a tracked widget's stylesheet template whenever it gets
    // reparented.  Widgets that have applyStyleSheet() called before
    // they're added to a layout (common — many widgets are configured
    // pre-parenting in helper functions) would otherwise resolve their
    // tokens against the WRONG scope chain (typically root, because
    // containerPathFor walks Qt's parent chain).  This filter catches
    // the subsequent QEvent::ParentChange and re-resolves so the
    // widget's containing applet / dialog scope finally reaches its
    // QSS — visible result: e.g. RF Power slider inside TxApplet
    // takes the applet/tx scope's red foreground instead of root blue.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Cleanup hook — fired when a widget tracked through applyStyleSheet
    // is destroyed.  Removes its entry from the reverse-map.
    void onTrackedWidgetDestroyed(QObject* obj);

private:
    ThemeManager();
    ~ThemeManager() override;   // out-of-line so std::unique_ptr<ThemeScope>
                                // member doesn't need the full type in this header
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

    // Serialize the current scope tree into AetherSDR's v2 theme JSON
    // (primitives + nested scopes) and write it to `path`.  Shared by
    // saveCurrentThemeAs (new user copy) and saveActiveTheme (rewrite
    // the active file in place).  Returns false if the file can't be
    // opened.
    bool writeThemeFile(const QString& themeName, const QString& path);

    // Built-in compiled-in defaults so a totally missing theme file
    // still produces a usable UI.  Populated in the constructor.
    void seedBuiltinDefaults();

    // Scope-tree helpers.
    //   * `scopeForPath(path)`   — returns the scope at `path` (nullptr
    //     if missing).  "" / "root" both map to the root scope.
    //   * `scopeOrCreate(path)`  — same, but creates the scope (and any
    //     missing parents) on demand.  Used by the scope-aware setters.
    //   * `resolveAlias(v)`      — if `v` is a `{primitive.key}` string,
    //     look it up in m_primitives and return that.  Otherwise pass
    //     `v` through unchanged.
    //   * `lookupRaw(path, key)` — walks the scope chain from `path` up
    //     to root, returning the first matching token (alias-resolved).
    //     Returns an invalid QVariant when nothing matches.
    ThemeScope* scopeForPath(const QString& path) const;
    ThemeScope* scopeOrCreate(const QString& path);
    QVariant    resolveAlias(const QVariant& v) const;
    QVariant    lookupRaw(const QString& containerPath, const QString& key) const;
    void        rebuildScopePathIndex();

    // JSON schema helpers.  v2 themes are `{schemaVersion:2, primitives:{},
    // scopes:{ root: { tokens:{}, scopes:{} } }}`.  v1 themes (no schema
    // version OR schemaVersion 1) carry a flat `tokens:{}` block that
    // migrates into the root scope on load.
    void        readPrimitivesFromJson(const QJsonObject& obj);
    void        readScopeFromJson(const QJsonObject& obj, ThemeScope* into);
    QJsonObject scopeToJson(const ThemeScope* scope) const;

    // Resource path or filesystem path indexed by theme display name.
    QHash<QString, QString> m_themePaths;

    // Token storage — a tree of scopes rooted at m_rootScope, with a
    // flat path → scope index for O(1) lookup.  Primitives live in a
    // separate flat map referenced via `{primitive.key}` aliases inside
    // scope tokens.  `m_tokens` is a reference into the root scope's
    // token hash so every legacy `m_tokens.foo` call site (96 of them
    // pre-refactor) compiles unchanged — the root scope IS the flat
    // namespace that existed before the scope tree was introduced.
    std::unique_ptr<ThemeScope>      m_rootScope;
    QHash<QString, ThemeScope*>      m_scopeByPath;
    QHash<QString, QVariant>         m_primitives;
    QHash<QString, QVariant>&        m_tokens;
    // Widget-declared container paths.  Persisted across theme loads so
    // a fresh JSON file (which only writes scopes that own overrides)
    // doesn't make a declared-but-unoverridden container vanish from
    // the editor's tree picker.  Re-installed into m_scopeByPath after
    // every loadThemeFromPath().
    QSet<QString>                    m_declaredContainers;
    QString m_activeTheme;

    // Factory-default snapshot, loaded once from `:/themes/default-dark.json`
    // at construction.  Drives the gradient editor's "Reset to default"
    // button.  Lazy-initialised so a totally missing resource bundle
    // doesn't take the whole singleton down.
    mutable QHash<QString, QVariant> m_factoryTokens;
    mutable bool m_factoryLoaded{false};
    void ensureFactoryLoaded() const;

    // Smart-invalidation hint — set transiently by setColor / setGradient
    // / setSizing / setString to the token that just changed, then
    // cleared after the synchronous themeChanged emission.  Lets
    // reapplyAllTrackedStyleSheets skip every widget whose template
    // doesn't reference that token, which is the 50–100x speedup that
    // makes a CompactColorPicker drag feel like 60fps live editing
    // instead of a stuck slideshow.
    QString m_currentEditToken;

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

// Declare a widget's container scope.  Stored as a Qt dynamic
// property ("themeContainer"); ThemeManager::containerPathFor() walks
// the Qt parent chain looking for the first declared ancestor so any
// child widget inherits its enclosing container automatically.
//
//   theme::setContainer(spectrumWidget, "spectrum");
//   theme::setContainer(panadapter,     "spectrum/panadapter");
//   // A QLabel inside panadapter with no declaration of its own
//   // resolves to "spectrum/panadapter" via the parent walk.
//
// Passing an empty path detaches the widget from any container,
// reverting it to root-scope lookups (also useful for tests).
void setContainer(QWidget* widget, const QString& containerPath);
QString containerOf(const QWidget* widget);
} // namespace theme

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ThemeGradient)
Q_DECLARE_METATYPE(AetherSDR::ThemeFont)
