#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QColor>
#include <QFont>
#include <QVariant>

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
    void themeChanged();

private:
    ThemeManager();
    ~ThemeManager() override = default;
    Q_DISABLE_COPY_MOVE(ThemeManager)

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
};

} // namespace AetherSDR
