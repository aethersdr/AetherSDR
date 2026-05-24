#include "ThemeManager.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace AetherSDR {

namespace {

// Recursively walk a JSON object, emitting `category.subkey...leaf = value`
// pairs into `out`.  Schema lets users group tokens under "color", "font",
// "sizing" without having to repeat the prefix at every leaf.
void flattenTokens(const QJsonObject& obj, const QString& prefix,
                   QHash<QString, QVariant>& out)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QString key = prefix.isEmpty() ? it.key()
                                             : prefix + QLatin1Char('.') + it.key();
        const QJsonValue v = it.value();
        if (v.isObject()) {
            // Plain nested object — keep recursing.  Gradient objects
            // (which also start with "type": "linear-gradient" etc.) land
            // in Phase 2; for now we ignore them on load so an early
            // theme file with a gradient still loads its scalars cleanly.
            const QJsonObject inner = v.toObject();
            if (inner.contains("type") && inner.value("type").isString()) {
                // Phase 2 will store gradients as a structured QVariant.
                // For Phase 1, log and skip so the rest of the theme loads.
                qCDebug(lcGui) << "ThemeManager: gradient token" << key
                                << "skipped (Phase 2 work)";
                continue;
            }
            flattenTokens(inner, key, out);
        } else if (v.isString()) {
            out.insert(key, v.toString());
        } else if (v.isDouble()) {
            out.insert(key, v.toDouble());
        } else if (v.isBool()) {
            out.insert(key, v.toBool());
        }
    }
}

} // namespace

ThemeManager& ThemeManager::instance()
{
    static ThemeManager s_instance;
    return s_instance;
}

ThemeManager::ThemeManager()
{
    seedBuiltinDefaults();
    scanAvailableThemes();

    const QString saved = AppSettings::instance()
                              .value("ActiveTheme", "Default Dark").toString();
    if (!setActiveTheme(saved)) {
        // Fall through to compiled-in defaults — UI still works, no theme
        // loaded.  Most commonly hit on a fresh install before the resource
        // bundle is in place.
        qCWarning(lcGui) << "ThemeManager: failed to load theme" << saved
                          << "— using compiled-in defaults";
    }
}

void ThemeManager::seedBuiltinDefaults()
{
    // Compiled-in defaults — the Phase 2 canonical taxonomy from
    // docs/theming/canonical-tokens.md.  Mirrors default-dark.json so
    // the UI is usable even with zero theme files on disk.  Kept in sync
    // with the JSON resource manually; Phase 5's editor will eventually
    // generate this table from the resource at compile time.

    // Backgrounds (6 tiers)
    m_tokens.insert("color.background.0",        QString("#0a0e14"));
    m_tokens.insert("color.background.1",        QString("#1a2a3a"));
    m_tokens.insert("color.background.2",        QString("#304050"));
    m_tokens.insert("color.background.3",        QString("#506070"));
    m_tokens.insert("color.background.tx",       QString("#3a2a0e"));
    m_tokens.insert("color.background.spectrum", QString("#000000"));

    // Accents
    m_tokens.insert("color.accent",          QString("#00b4d8"));
    m_tokens.insert("color.accent.bright",   QString("#00c8f0"));
    m_tokens.insert("color.accent.dim",      QString("#0090e0"));
    m_tokens.insert("color.accent.warning",  QString("#ffb84d"));
    m_tokens.insert("color.accent.danger",   QString("#ff4d4d"));
    m_tokens.insert("color.accent.success",  QString("#4dd87a"));

    // Text (4 tiers — label and disabled distinct for Phase 4 contrast tuning)
    m_tokens.insert("color.text.primary",   QString("#e6f0fa"));
    m_tokens.insert("color.text.secondary", QString("#8ea8c0"));
    m_tokens.insert("color.text.label",     QString("#506070"));
    m_tokens.insert("color.text.disabled",  QString("#3a4a5a"));

    // Borders
    m_tokens.insert("color.border.subtle", QString("#1a2330"));
    m_tokens.insert("color.border.strong", QString("#2a3a4d"));
    m_tokens.insert("color.border.accent", QString("#00b4d8"));
    m_tokens.insert("color.border.tx",     QString("#5a4a28"));

    // Meters (paint code only)
    m_tokens.insert("color.meter.crst",          QString("#ff4d4d"));
    m_tokens.insert("color.meter.rms",           QString("#00b4d8"));
    m_tokens.insert("color.meter.thresh",        QString("#ffb84d"));
    m_tokens.insert("color.meter.peak",          QString("#e6f0fa"));
    m_tokens.insert("color.meter.gainReduction", QString("#f2c14e"));
    m_tokens.insert("color.meter.bar.fill",      QString("#405060"));

    // Spectrum + waterfall (paint code only — gradient waterfall.colormap
    // lands when gradient-token support follows this PR)
    m_tokens.insert("color.spectrum.trace",    QString("#00b4d8"));
    m_tokens.insert("color.spectrum.peakHold", QString("#ffb84d"));
    m_tokens.insert("color.spectrum.average",  QString("#8ea8c0"));
    m_tokens.insert("color.spectrum.grid",     QString("#1a2330"));

    // Slice indicators A-H + TX-active highlight.  Preliminary values —
    // a dedicated slice-colour audit may tune these in a follow-up.
    m_tokens.insert("color.slice.a",  QString("#ff4040"));
    m_tokens.insert("color.slice.b",  QString("#ff8c00"));
    m_tokens.insert("color.slice.c",  QString("#ffd040"));
    m_tokens.insert("color.slice.d",  QString("#40c060"));
    m_tokens.insert("color.slice.e",  QString("#00b4d8"));
    m_tokens.insert("color.slice.f",  QString("#4080ff"));
    m_tokens.insert("color.slice.g",  QString("#c060ff"));
    m_tokens.insert("color.slice.h",  QString("#ff60a0"));
    m_tokens.insert("color.slice.tx", QString("#ff4d4d"));

    // Font + sizing (unchanged from Phase 1 seed)
    m_tokens.insert("font.family.ui",       QString("Inter"));
    m_tokens.insert("font.family.mono",     QString("monospace"));
    m_tokens.insert("font.size.tiny",       9);
    m_tokens.insert("font.size.small",      10);
    m_tokens.insert("font.size.normal",     12);
    m_tokens.insert("font.size.large",      14);
    m_tokens.insert("sizing.panel.padding",      4);
    m_tokens.insert("sizing.panel.spacing",      4);
    m_tokens.insert("sizing.panel.cornerRadius", 4);
    m_tokens.insert("sizing.border.subtle",      1);
    m_tokens.insert("sizing.border.strong",      2);
}

void ThemeManager::scanAvailableThemes()
{
    // Built-ins: scan :/themes/ in the Qt resource system.  Bundled
    // themes land here via resources/resources.qrc.
    {
        QDir d(":/themes/");
        const auto entries = d.entryList({"*.json"}, QDir::Files);
        for (const QString& file : entries) {
            const QString full = ":/themes/" + file;
            QFile f(full);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
            f.close();
            if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
            const QString name = doc.object().value("name").toString();
            if (!name.isEmpty()) m_themePaths.insert(name, full);
        }
    }

    // User themes: ~/.config/AetherSDR/themes/ on Linux, equivalent on
    // other platforms via QStandardPaths.  Loaded only if the directory
    // exists — Phase 1 doesn't create it (Phase 5's editor does on first
    // save).
    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                                + QStringLiteral("/themes");
    QDir d(userDir);
    if (d.exists()) {
        const auto entries = d.entryList({"*.json"}, QDir::Files);
        for (const QString& file : entries) {
            const QString full = d.absoluteFilePath(file);
            QFile f(full);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
            f.close();
            if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
            const QString name = doc.object().value("name").toString();
            if (!name.isEmpty()) m_themePaths.insert(name, full);
        }
    }
}

bool ThemeManager::loadThemeFromPath(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(lcGui) << "ThemeManager: cannot open" << path;
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError) {
        qCWarning(lcGui) << "ThemeManager: parse error in" << path
                          << ":" << err.errorString();
        return false;
    }
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();

    const int schemaVersion = root.value("schemaVersion").toInt(0);
    if (schemaVersion < 1) {
        qCWarning(lcGui) << "ThemeManager: unsupported schemaVersion"
                          << schemaVersion << "in" << path;
        return false;
    }

    // Compiled-in defaults stay as the fallback layer; tokens defined in
    // the file overwrite them.  This is how older theme files with fewer
    // tokens still produce a fully-rendered UI on a newer build.
    QHash<QString, QVariant> newTokens;
    seedBuiltinDefaults();  // reset to defaults
    newTokens = m_tokens;
    flattenTokens(root.value("tokens").toObject(), QString(), newTokens);
    m_tokens.swap(newTokens);
    return true;
}

QStringList ThemeManager::availableThemes() const
{
    QStringList names = m_themePaths.keys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString ThemeManager::activeTheme() const
{
    return m_activeTheme;
}

bool ThemeManager::setActiveTheme(const QString& name)
{
    if (name == m_activeTheme && !m_activeTheme.isEmpty()) return true;
    const auto it = m_themePaths.constFind(name);
    if (it == m_themePaths.constEnd()) {
        qCDebug(lcGui) << "ThemeManager: theme" << name << "not found";
        return false;
    }
    if (!loadThemeFromPath(it.value())) return false;
    m_activeTheme = name;
    AppSettings::instance().setValue("ActiveTheme", name);
    AppSettings::instance().save();
    emit themeChanged();
    return true;
}

QColor ThemeManager::color(const QString& token) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) {
        qCWarning(lcGui) << "ThemeManager: missing color token" << token;
        return QColor(Qt::transparent);
    }
    return QColor(it.value().toString());
}

QFont ThemeManager::font(const QString& token) const
{
    // Convention: font.* tokens are read as a (family, size) compound
    // when the caller asks for a font object.  Sub-tokens (family / size
    // / weight) come from sibling tokens — caller passes the *base*
    // token (e.g. "font" for the UI default) and we assemble from
    // "font.family.ui" + "font.size.normal".  Phase 1 ships only the
    // direct read for the simplest path; richer font composition lands
    // when Phase 5's font picker arrives.
    QFont f;
    const QString family = value("font.family.ui");
    if (!family.isEmpty()) f.setFamily(family);
    f.setPointSize(sizing(token));
    return f;
}

int ThemeManager::sizing(const QString& token) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) {
        qCWarning(lcGui) << "ThemeManager: missing sizing token" << token;
        return 0;
    }
    bool ok = false;
    const int v = it.value().toInt(&ok);
    if (ok) return v;
    return static_cast<int>(it.value().toDouble());
}

QString ThemeManager::value(const QString& token) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) return QString();
    return it.value().toString();
}

QString ThemeManager::resolve(const QString& stylesheetTemplate) const
{
    // Replace every {{token.name}} with the token's stylesheet fragment.
    // Today: scalar colour tokens emit "#rrggbb"; numeric tokens emit
    // their value as a plain string ("12" -> "12px" is the caller's
    // responsibility).  Phase 2 routes gradient tokens through
    // cssFragment() and emits qlineargradient(...) directly.
    static const QRegularExpression kRe(QStringLiteral(R"(\{\{([^}]+)\}\})"));
    QString out = stylesheetTemplate;
    QRegularExpressionMatchIterator it = kRe.globalMatch(stylesheetTemplate);
    int offset = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString token = m.captured(1).trimmed();
        const QString val = value(token);
        // Adjust for the running offset as substitutions change length.
        out.replace(m.capturedStart(0) + offset,
                    m.capturedLength(0),
                    val);
        offset += (val.length() - m.capturedLength(0));
    }
    return out;
}

} // namespace AetherSDR
