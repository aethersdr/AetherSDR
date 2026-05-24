#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QColor>
#include <QFont>
#include <QVariant>

class QWidget;

namespace AetherSDR {

// Token-based theming subsystem (RFC #3076 Phase 1).
//
// Every visual decision in the GUI — colours, fonts, key spacings —
// resolves through a named token (e.g. "color.accent", "font.size.normal").
// Themes are JSON files at ~/.config/AetherSDR/themes/<name>.json plus the
// built-in default-dark / default-light shipped under :/themes/.
//
// Phase 1 ships:
//   - the manager singleton + token API
//   - JSON loader (scalar values only — gradient support lands in Phase 2)
//   - stylesheet template resolver ({{token.name}} substitution)
//   - active-theme persistence via AppSettings (ActiveTheme key)
//   - default-dark.json baked into Qt resources, bit-identical to today's
//     hardcoded palette so v0 ships with zero visual diff
//
// Phase 2 will: add the migration audit tool, convert shared stylesheets
// to the template form, and start recording the widget→token reverse-map
// for the eventual inspector-mode editor.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    // Token accessors.  Missing tokens log a warning and return the
    // compiled-in default for the type (transparent black / default
    // QFont / 0).  Phase 5's editor will surface missing-token warnings
    // to the user; for now they're warning logs only.
    QColor   color(const QString& token) const;
    QFont    font(const QString& token) const;
    int      sizing(const QString& token) const;
    QString  value(const QString& token) const;   // raw token resolution

    // Stylesheet template resolver.  Replaces every "{{token.name}}"
    // placeholder with the corresponding token's stylesheet fragment
    // (today: #rrggbb for colours, raw value for sizing).  Phase 2 will
    // add gradient tokens emitting qlineargradient(...) syntax.
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

    // Phase 1 doesn't implement save / import / export — those land with
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
