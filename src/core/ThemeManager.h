#pragma once

#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMutex>
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

// Token-based theming (RFC #3076). Every visual decision in the GUI resolves
// through a named token (e.g. "color.accent", "font.size.normal"). Themes are
// JSON in ~/.config/AetherSDR/themes/<name>.json plus built-in default-dark /
// default-light under :/themes/. A color token is a scalar (#rrggbb) or a
// linear/radial gradient with N stops: brush() returns a QBrush, cssFragment()
// emits qlineargradient/qradialgradient, and resolve() routes through
// cssFragment() so {{token}} templating works for gradients.

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

// Compound font token for a typographic role (`font.family.*` namespace).
// JSON: { "family": "Inter", "size": 12, "color": "#c8d8e8" }; family is
// required, size 0 = caller's role default, invalid color = fall back to
// color.text.primary. value(token) on a ThemeFont returns .family.
struct ThemeFont {
    QString family;
    int     size  {0};   // 0 = unset
    QColor  color;       // invalid = unset
};

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    // Scalar accessors. Missing tokens warn and return the type's compiled-in
    // default; color() on a gradient returns its first stop (use brush() or
    // cssFragment() for the gradient).
    //   * `color(token)` — root-scope lookup.
    //   * `color(widget, token)` — starts at the first ancestor with a
    //     `themeContainer` property and walks the scope tree to root; root lookup
    //     if no ancestor declares one.
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

    // Apply a stylesheet template and record widget → referenced tokens (used by
    // the inspector). Registered widgets are re-applied with fresh values on
    // themeChanged; the entry is removed on QObject::destroyed.
    void applyStyleSheet(QWidget* widget, const QString& stylesheetTemplate);

    // Temporarily own a standard widget's text foreground without replacing its
    // base stylesheet. Empty token restores the latest base style. Both layers
    // remain theme/scope aware; subsequent applyStyleSheet calls preserve this
    // treatment. Custom paint code must consume the token itself.
    void setWidgetForegroundToken(QWidget* widget, const QString& token);

    // Shared QCheckBox::indicator style fragment — ThemeManager tokens plus
    // the full hover/checked/disabled pseudo-state set — so every dialog gets
    // a visible, theme-reactive indicator in dark mode without hand-rolling
    // the block per file (#4013).  Concatenate it onto the caller's
    // "QCheckBox { ... }" template and pass the result to applyStyleSheet();
    // it returns an unresolved template, so setStyleSheet() would NOT expand
    // the {{tokens}} — use applyStyleSheet().
    static QString checkBoxIndicatorStyle();

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

    // Sub-region inspector lookup for custom-paint widgets: each ThemeRegion ties a
    // token to a hit test in widget-local coordinates; tokensAtPoint() narrows
    // declareWidgetTokens() to the clicked region and returns all matches in
    // declaration order.
    //   tm.declareWidgetRegions(spectrum, {
    //     { "color.spectrum.trace",      [this](QPoint p){ return panRect().contains(p); }, "FFT trace" },
    //     { "color.waterfall.colormap",  [this](QPoint p){ return wfRect().contains(p);  }, "Waterfall" },
    //   });
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

    // Factory-default lookups — read from a snapshot of the bundled theme
    // the ACTIVE theme descends from (`default-light.json` when editing a
    // light theme, `default-dark.json` otherwise), so every Reset-to-default
    // affordance in the editor restores the canonical value *for that base*.
    // The snapshot re-loads when the active theme changes base.  Each returns
    // a sentinel (empty / invalid / 0 / -1) when the token has no factory
    // baseline — callers should check before using.
    ThemeGradient factoryGradient(const QString& token) const;
    QColor        factoryColor(const QString& token) const;
    int           factorySizing(const QString& token) const;     // -1 = none
    QString       factoryString(const QString& token) const;
    bool          hasFactoryValue(const QString& token) const;

    // Theme-file management — Delete / Rename for the user's saved themes
    // living under QStandardPaths::GenericConfigLocation + "/AetherSDR/themes"
    // (`~/.config/...` on Linux, `%LOCALAPPDATA%` on Windows, `~/Library/
    // Preferences` on macOS).  Both refuse on built-in themes (those live
    // inside the Qt resource bundle and aren't deletable).  Delete switches
    // the active theme back to "Default Dark" before unlinking so the UI
    // doesn't render half-blank during the file removal.
    bool        deleteTheme(const QString& name);
    bool        renameTheme(const QString& oldName, const QString& newName);
    bool        isBuiltInTheme(const QString& name) const;

    // Is this a name we can turn into a theme FILE?  saveCurrentThemeAs() and
    // renameTheme() both enforce it and both REFUSE rather than substitute,
    // because at those two entry points the operator typed the name.  Public
    // so the editor can check before it asks and report the actual reason —
    // a refusal the UI can only describe as "couldn't write the file" is worse
    // than useless, it sends the operator to check directory permissions.
    // `reason` (optional) receives operator-facing text.
    static bool isValidThemeName(const QString& name, QString* reason = nullptr);

    bool        saveCurrentThemeAs(const QString& newThemeName);

    // Persist the current in-memory token state back to the active
    // theme's on-disk file.  No-op for built-in themes (their path
    // points at the read-only resource bundle) and for unknown
    // themes.  Called automatically by setColor / setSizing /
    // setGradient / setString so edits survive a restart without
    // requiring an explicit "Save" gesture.
    bool        saveActiveTheme();

    // `.aethertheme` is plain JSON (the shape saveCurrentThemeAs writes) with a
    // `schemaVersion`. On import, missing tokens fall back to built-in defaults and
    // unknown tokens round-trip unchanged.
    // exportThemeToFile(): `themeName` as registered (activeTheme() for live
    //   state); `filePath` absolute; the caller owns the dialog.
    // importThemeFromFile(): validates magic + schema, names the theme from JSON
    //   "name" (else file stem), copies it into ~/.config/AetherSDR/themes/,
    //   registers and activates it. Returns the display name, or empty with
    //   `errorMessage` set.
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
    // Re-resolve a tracked widget's template on reparent/polish/show, since
    // applyStyleSheet() is often called before the widget joins its scoped
    // container and would otherwise resolve at root (see the .cpp for the cases).
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

    // Assemble the v2 theme document (schemaVersion + metadata + primitives
    // palette + nested scope tree) for the live theme state.  THE single
    // document builder: writeThemeFile() and exportThemeToFile() both go
    // through it so the two can't emit structurally different files.  In
    // particular `primitives` and `scopes` can only ever travel together —
    // scope tokens hold `{primitive.key}` aliases verbatim, so scopes without
    // the palette they point into load as invalid colours.
    QJsonObject themeDocumentJson(const QString& themeName,
                                  const QString& description) const;

    // Serialize the current scope tree into AetherSDR's v2 theme JSON
    // (primitives + nested scopes) and write it to `path`.  Shared by
    // saveCurrentThemeAs (new user copy) and saveActiveTheme (rewrite
    // the active file in place).  Returns false if the file can't be
    // opened.
    bool writeThemeFile(const QString& themeName, const QString& path);

    // Built-in compiled-in defaults so a totally missing theme file
    // still produces a usable UI.  Populated in the constructor.
    void seedBuiltinDefaults();
    // Generated from resources/themes/default-dark.json by
    // tools/gen_theme_seed.py; defined in ThemeSeedGenerated.cpp. Never edit
    // that file by hand — regenerate it. (#3184)
    void seedGeneratedDefaults();
    // Seed one token into a scope, creating the scope if needed.
    //
    // Exists so the generated translation unit never has to see ThemeScope,
    // whose definition is private to ThemeManager.cpp. Silent by design: this
    // runs during construction, before any consumer could be connected, so
    // emitting themeChanged() per token would be both pointless and 128 signals
    // deep. The public setColor()/setSizing() overloads are the notifying path.
    void seedScopedToken(const QString& containerPath,
                         const QString& token, const QVariant& value);

    // Scope-tree helpers:
    //   * scopeForPath(path)   — scope at `path` or nullptr; "" and "root" = root.
    //   * scopeOrCreate(path)  — same, creating missing scopes.
    //   * resolveAlias(v)      — `{primitive.key}` → m_primitives value, else v.
    //   * lookupRaw(path, key) — first alias-resolved match from `path` up to root;
    //     invalid QVariant if none.
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

    // Tokens already warned about by cssFragment(), so a stylesheet typo is
    // reported once rather than on every theme change and every tracked-
    // stylesheet reapply.  Cleared on every theme load, so the warning tracks
    // the theme it's about instead of latching for the process.  Mutable
    // because cssFragment() is const; the mutex guards only this set, which is
    // the whole of ThemeManager's locking — every other member is
    // main-thread-only, as resolveFor()'s callers all terminate in
    // QWidget::setStyleSheet.
    mutable QSet<QString>            m_warnedUnknownTokens;
    mutable QMutex                   m_unknownTokenMutex;

    // Factory-default snapshot of whichever bundled theme the active theme
    // descends from (see factoryBaselinePath()).  Drives every "Reset to
    // default" affordance in the editor.  Lazy-initialised so a totally
    // missing resource bundle doesn't take the whole singleton down, and
    // latched only on a SUCCESSFUL load so one failed read doesn't disable
    // Reset for the rest of the process.
    mutable QHash<QString, QVariant> m_factoryTokens;
    mutable bool m_factoryLoaded{false};
    // Which bundled theme the current snapshot came from, so a Dark -> Light
    // switch re-snapshots instead of serving the previous base's values.
    mutable QString m_factoryBaselinePath;
    void ensureFactoryLoaded() const;
    // Bundled theme the active theme's "factory default" should come from.
    QString factoryBaselinePath() const;

    // "Default Light" / "Default Dark" — the bundled theme the ACTIVE theme is
    // a descendant of.  Decided once per theme load by resolveThemeBase() and
    // then held constant, because the thing that reads it is the Reset button
    // and the operator presses Reset when a value is already wrong: deriving
    // it from live token state lets a light theme whose background has been
    // dragged dark reclassify itself, and then Reset hands back dark values.
    QString m_activeThemeBase;
    // From recorded parentage (`baseTheme` in the document) where present,
    // falling back to the freshly-loaded background's luminance where not.
    QString resolveThemeBase(const QJsonObject& root) const;

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
        QString             foregroundToken;
        QString effectiveTemplate(const QWidget* widget) const;
        // Exactly the QSS *we* last pushed onto the widget.  If the widget's
        // current stylesheet still equals this, nobody has overridden us and
        // a re-resolve is safe.  If it differs, a caller set its own sheet
        // afterwards — per-slice badge colours, TX indicator state, RADE SNR
        // colour — and re-resolving would silently wipe it.  See eventFilter.
        QString             appliedStylesheet;
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

// Declare a widget's container scope (dynamic property "themeContainer");
// children inherit it via containerPathFor()'s parent walk.
//   theme::setContainer(spectrumWidget, "spectrum");
//   theme::setContainer(panadapter,     "spectrum/panadapter");
// An empty path detaches the widget (root-scope lookups).
void setContainer(QWidget* widget, const QString& containerPath);
QString containerOf(const QWidget* widget);
} // namespace theme

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ThemeGradient)
Q_DECLARE_METATYPE(AetherSDR::ThemeFont)
