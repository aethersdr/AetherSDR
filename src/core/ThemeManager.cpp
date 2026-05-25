#include "ThemeManager.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QWidget>
#include <QtMath>
#include <cmath>

namespace AetherSDR {

namespace {

// Parse a JSON gradient object into the structured ThemeGradient form.
// Recognised schema (linear):
//   { "type": "linear-gradient", "angle": 180,
//     "stops": [ { "at": 0.0, "color": "#aabbcc" }, ... ] }
// Recognised schema (radial):
//   { "type": "radial-gradient", "center": [0.5, 0.5], "radius": 0.7,
//     "stops": [ ... ] }
// Unknown "type" values default to Linear with an empty stop list — the
// downstream brush()/cssFragment() handles empty-stop gradients as
// transparent black.
ThemeGradient parseGradient(const QJsonObject& obj)
{
    ThemeGradient g;
    const QString type = obj.value("type").toString();
    g.type = (type == QLatin1String("radial-gradient")) ? ThemeGradient::Radial
                                                        : ThemeGradient::Linear;
    g.angle = obj.value("angle").toDouble(180.0);
    if (g.type == ThemeGradient::Radial) {
        const QJsonArray c = obj.value("center").toArray();
        if (c.size() >= 2) {
            g.center = QPointF(c.at(0).toDouble(0.5), c.at(1).toDouble(0.5));
        }
        g.radius = obj.value("radius").toDouble(0.5);
    }
    const QJsonArray stops = obj.value("stops").toArray();
    g.stops.reserve(stops.size());
    for (const QJsonValue& v : stops) {
        const QJsonObject so = v.toObject();
        ThemeGradientStop s;
        s.at = so.value("at").toDouble(0.0);
        s.color = QColor(so.value("color").toString());
        g.stops.append(s);
    }
    return g;
}

// Round-trip-safe hex encoding for token storage.  Qt's QColor::name()
// defaults to "#rrggbb" and silently drops alpha — so any caller storing
// `color.name()` loses translucency.  Always use the explicit format that
// keeps alpha when present.
QString colorToTokenString(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb)
                            : c.name(QColor::HexArgb);
}

// Convert a stored hex string ("#rrggbb" or "#aarrggbb") to a Qt
// stylesheet fragment.  Qt's stylesheet parser doesn't accept the 8-digit
// "#aarrggbb" form, so we route translucent colours through rgba(...)
// instead.  Opaque colours pass through verbatim — preserves the diffable
// rendering output every test asserts against.
QString colorHexToCssFragment(const QString& hex)
{
    const QColor c(hex);
    if (!c.isValid() || c.alpha() == 255) return hex;
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(c.alphaF(), 0, 'f', 3);
}

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
            const QJsonObject inner = v.toObject();
            // Gradient objects carry a "type" string discriminator that
            // distinguishes them from plain nested token groups.
            if (inner.contains("type") && inner.value("type").isString()) {
                out.insert(key, QVariant::fromValue(parseGradient(inner)));
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

// Convert a CSS-convention angle (0deg = bottom→top, 90deg = left→right,
// 180deg = top→bottom, 270deg = right→left) to a 0–1 normalised
// (x1, y1, x2, y2) endpoint pair that covers a unit square.
//
// Used by both brush() (mapping onto a real QRect) and cssFragment()
// (emitting normalised stylesheet coords).  Extracted so the two paths
// stay in lock-step — a future bug in one would otherwise produce subtle
// mismatches between the painted gradient and the stylesheet-emitted
// preview.
void linearAngleToEndpoints(qreal angleDeg,
                            qreal& x1, qreal& y1, qreal& x2, qreal& y2)
{
    const qreal rad = qDegreesToRadians(angleDeg);
    // Half-diagonal of the unit square — guarantees the gradient line
    // reaches the corners regardless of angle.  sqrt(2)/2 ≈ 0.7071.
    constexpr qreal kHalfDiagonal = 0.7071067811865476;
    const qreal dx = std::sin(rad);
    const qreal dy = -std::cos(rad);  // CSS Y axis is inverted vs screen
    x1 = 0.5 - dx * kHalfDiagonal;
    y1 = 0.5 - dy * kHalfDiagonal;
    x2 = 0.5 + dx * kHalfDiagonal;
    y2 = 0.5 + dy * kHalfDiagonal;
}

// Emit a Qt stylesheet gradient fragment for the given gradient.
QString gradientCssFragment(const ThemeGradient& g)
{
    QString out;
    if (g.type == ThemeGradient::Linear) {
        qreal x1, y1, x2, y2;
        linearAngleToEndpoints(g.angle, x1, y1, x2, y2);
        out = QStringLiteral("qlineargradient(x1:%1, y1:%2, x2:%3, y2:%4")
                  .arg(x1, 0, 'f', 4).arg(y1, 0, 'f', 4)
                  .arg(x2, 0, 'f', 4).arg(y2, 0, 'f', 4);
    } else {
        out = QStringLiteral("qradialgradient(cx:%1, cy:%2, radius:%3, "
                             "fx:%1, fy:%2")
                  .arg(g.center.x(), 0, 'f', 4)
                  .arg(g.center.y(), 0, 'f', 4)
                  .arg(g.radius,     0, 'f', 4);
    }
    for (const auto& s : g.stops) {
        out += QStringLiteral(", stop:%1 %2")
                   .arg(s.at, 0, 'f', 4)
                   .arg(colorHexToCssFragment(colorToTokenString(s.color)));
    }
    out += QLatin1Char(')');
    return out;
}

} // namespace

ThemeManager& ThemeManager::instance()
{
    static ThemeManager s_instance;
    return s_instance;
}

ThemeManager::ThemeManager()
{
    // Explicit metatype registration so qMetaTypeId<ThemeGradient>() and
    // direct userType comparisons resolve correctly even before the first
    // QVariant::fromValue<ThemeGradient>() call.  Q_DECLARE_METATYPE in
    // the header sets up the template machinery, but explicit registration
    // here makes saveCurrentThemeAs()'s type check timing-independent.
    qRegisterMetaType<ThemeGradient>("AetherSDR::ThemeGradient");

    seedBuiltinDefaults();
    scanAvailableThemes();

    // Stylesheet-painted widgets registered via applyStyleSheet() get
    // free live-reload on theme change — re-resolve the template, push
    // the new stylesheet, no consumer plumbing required.
    connect(this, &ThemeManager::themeChanged,
            this, &ThemeManager::reapplyAllTrackedStyleSheets);

    const QString saved = AppSettings::instance()
                              .value("ActiveTheme", "Default Dark").toString();
    if (!setActiveTheme(saved)) {
        // Saved theme is gone (most commonly: user saved a custom theme
        // via the editor and later removed the file out-of-band).  Don't
        // limp along on seedBuiltinDefaults() — its scalar-only token set
        // lacks the waterfall.colormap gradients and the operator gets a
        // baffling all-grayscale waterfall.  Fall back to "Default Dark"
        // explicitly so the bundled JSON loads and every token is populated.
        qCWarning(lcGui) << "ThemeManager: saved theme" << saved
                          << "is unavailable — falling back to Default Dark";
        if (!setActiveTheme(QStringLiteral("Default Dark"))) {
            qCWarning(lcGui) << "ThemeManager: Default Dark also failed to load"
                              << "— UI will render with compiled-in defaults"
                              << "(rebuild resources?)";
        }
    }
}

void ThemeManager::seedBuiltinDefaults()
{
    // Compiled-in defaults — the Phase 2 canonical taxonomy from
    // docs/theming/canonical-tokens.md.  Mirrors default-dark.json so
    // the UI is usable even with zero theme files on disk.  Kept in sync
    // with the JSON resource manually; Phase 5's editor will eventually
    // generate this table from the resource at compile time.

    // Backgrounds (6 tiers).  background.0 is the dominant codebase
    // QWidget base (#0f0f1a, 84 refs); aligning the canonical token to
    // that value makes the migration bit-identical at every QWidget
    // that doesn't paint its own background.
    m_tokens.insert("color.background.0",        QString("#0f0f1a"));
    m_tokens.insert("color.background.1",        QString("#1a2a3a"));
    m_tokens.insert("color.background.2",        QString("#304050"));
    m_tokens.insert("color.background.3",        QString("#506070"));
    m_tokens.insert("color.background.tx",       QString("#3a2a0e"));
    m_tokens.insert("color.background.success",  QString("#006040"));
    m_tokens.insert("color.background.spectrum", QString("#000000"));
    // App-level backdrop painted by MainWindow itself.  Honours alpha for
    // the "fade to desktop" experiment — when this token's value is
    // translucent, the compositor renders the desktop wallpaper through
    // any pixels the rest of the app didn't claim.  Opaque by default so
    // existing installs see no visual change.
    m_tokens.insert("color.background.app",      QString("#0f0f1a"));

    // Accents
    m_tokens.insert("color.accent",          QString("#00b4d8"));
    m_tokens.insert("color.accent.bright",   QString("#00c8f0"));
    m_tokens.insert("color.accent.dim",      QString("#0090e0"));
    m_tokens.insert("color.accent.warning",  QString("#ffb84d"));
    m_tokens.insert("color.accent.danger",   QString("#ff4d4d"));
    m_tokens.insert("color.accent.success",  QString("#4dd87a"));

    // Text (4 tiers — label and disabled distinct for Phase 4 contrast
    // tuning).  text.primary aligned to the dominant codebase body-text
    // value (#c8d8e8, 367 refs across applets / dialogs / labels).
    m_tokens.insert("color.text.primary",   QString("#c8d8e8"));
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
    // Vertical (bottom→top, angle 0°) green→amber→red ramp painted by
    // ClientLevelMeter / ClientCompMeter into the level bar.  Seeded so
    // a missing theme file doesn't fall back to whatever the meter
    // widgets used to hardcode — the editor expects this token to always
    // exist as a gradient.
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 0.0;
        g.stops = {
            {0.00, QColor("#2f9e6a")},
            {0.55, QColor("#6cc56a")},
            {0.80, QColor("#e8b94c")},
            {0.95, QColor("#e8553c")},
            {1.00, QColor("#f2362a")},
        };
        m_tokens.insert("color.meter.bar.fillGradient",
                        QVariant::fromValue(g));
    }

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
    //
    // Built-in names are RESERVED — user-dir files with a colliding name
    // are skipped with a warning rather than allowed to shadow the
    // bundled theme.  Otherwise a stale user-dir copy of "Default Dark"
    // saved before a schema change (e.g. the Phase 3 waterfall colormap
    // restructure that broke flat → nested gradient layout) silently
    // overrides the corrected bundled version and produces baffling
    // partial-render bugs.  Users wanting a tweaked version should Save
    // As under a new name through the editor.
    // Use GenericConfigLocation + "/AetherSDR" so the path is ~/.config/AetherSDR/themes,
    // not the double-nested ~/.config/AetherSDR/AetherSDR/themes that
    // AppConfigLocation produces when both org and app names are "AetherSDR".
    // Matches the convention AppSettings and the log dir already use.
    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                + QStringLiteral("/AetherSDR/themes");
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
            if (name.isEmpty()) continue;
            const auto existing = m_themePaths.constFind(name);
            if (existing != m_themePaths.constEnd()
                && existing.value().startsWith(QStringLiteral(":/themes/"))) {
                qCWarning(lcGui) << "ThemeManager: ignoring user-dir theme"
                                 << full << "— name collides with built-in"
                                 << name << "(use Save As under a new name)";
                continue;
            }
            m_themePaths.insert(name, full);
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

QStringList ThemeManager::allTokenKeys() const
{
    QStringList keys = m_tokens.keys();
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

void ThemeManager::setColor(const QString& token, const QColor& color)
{
    if (!color.isValid()) return;
    // Live-edit path stores the QColor as a hex string so cssFragment() /
    // resolve() pick it up the same way they pick up freshly-loaded tokens.
    // Use colorToTokenString() — bare QColor::name() drops alpha, which
    // breaks rgba editing through the picker.
    // Skip the emit if nothing actually changed (avoids burning a re-paint
    // cycle on a no-op edit).
    const QString hex = colorToTokenString(color);
    const auto it = m_tokens.constFind(token);
    if (it != m_tokens.constEnd() && it.value().toString() == hex) return;
    m_tokens.insert(token, QVariant(hex));
    emit themeChanged();
}

void ThemeManager::setSizing(const QString& token, int value)
{
    const auto it = m_tokens.constFind(token);
    if (it != m_tokens.constEnd() && it.value().toInt() == value) return;
    m_tokens.insert(token, QVariant(value));
    emit themeChanged();
}

ThemeGradient ThemeManager::gradient(const QString& token) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) return {};
    if (!it.value().canConvert<ThemeGradient>()) return {};
    return it.value().value<ThemeGradient>();
}

void ThemeManager::setGradient(const QString& token, const ThemeGradient& g)
{
    m_tokens.insert(token, QVariant::fromValue(g));
    emit themeChanged();
}

void ThemeManager::ensureFactoryLoaded() const
{
    if (m_factoryLoaded) return;
    m_factoryLoaded = true;  // one shot — even if loading fails, don't re-try
    QFile f(QStringLiteral(":/themes/default-dark.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(lcGui) << "ThemeManager: factory snapshot — failed to open"
                         << f.fileName();
        return;
    }
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcGui) << "ThemeManager: factory snapshot — JSON parse error"
                         << err.errorString();
        return;
    }
    flattenTokens(doc.object().value("tokens").toObject(), QString(),
                  m_factoryTokens);
}

ThemeGradient ThemeManager::factoryGradient(const QString& token) const
{
    ensureFactoryLoaded();
    const auto it = m_factoryTokens.constFind(token);
    if (it == m_factoryTokens.constEnd()) return {};
    if (!it.value().canConvert<ThemeGradient>()) return {};
    return it.value().value<ThemeGradient>();
}

bool ThemeManager::saveCurrentThemeAs(const QString& newThemeName)
{
    if (newThemeName.trimmed().isEmpty()) return false;

    // Round-trip-safe flat dotted-key serialization.  The bundled themes
    // organize tokens into nested objects for human readability, but the
    // earlier deep-walk version of this code had two bugs that lost data
    // when it tried to reproduce that layout:
    //
    //   * Prefix-conflict — a scalar like `color.accent` collided with
    //     nested children like `color.accent.bright`; whichever was
    //     processed second clobbered the first.
    //   * Gradient tokens were silently skipped via a fragile
    //     canConvert<ThemeGradient>() check (false negatives observed
    //     under some metatype-registration timings).
    //
    // The flat output below is a strict subset of what flattenTokens()
    // accepts (`{"tokens": {"color.background.0": "#0f0f1a", ...}}`):
    // each dotted key becomes a literal JSON key inside `tokens`.  Less
    // pretty than the bundled themes, but every token round-trips
    // perfectly with zero conflict surface.  Future-Phase-5 polish can
    // group these into the bundled nested layout once the editor is
    // doing more than dump-and-reload.
    QJsonObject tokensObj;
    const int gradMetaId = qMetaTypeId<ThemeGradient>();
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it) {
        const QVariant& v = it.value();
        const int ut = v.userType();
        QJsonValue leaf;
        if (ut == QMetaType::QString)      leaf = v.toString();
        else if (ut == QMetaType::Int)     leaf = v.toInt();
        else if (ut == QMetaType::Double)  leaf = v.toDouble();
        else if (ut == QMetaType::Bool)    leaf = v.toBool();
        else if (ut == gradMetaId) {
            const ThemeGradient g = v.value<ThemeGradient>();
            QJsonObject gj;
            gj.insert("type", g.type == ThemeGradient::Radial
                                ? QStringLiteral("radial-gradient")
                                : QStringLiteral("linear-gradient"));
            gj.insert("angle", g.angle);
            if (g.type == ThemeGradient::Radial) {
                gj.insert("centerX", g.center.x());
                gj.insert("centerY", g.center.y());
                gj.insert("radius",  g.radius);
            }
            QJsonArray stops;
            for (const auto& s : g.stops) {
                QJsonObject sj;
                sj.insert("at",    s.at);
                sj.insert("color", colorToTokenString(s.color));
                stops.append(sj);
            }
            gj.insert("stops", stops);
            leaf = gj;
        }
        else {
            qCWarning(lcGui) << "ThemeManager::saveCurrentThemeAs: skipping token"
                             << it.key() << "with unsupported metatype" << ut;
            continue;
        }
        tokensObj.insert(it.key(), leaf);
    }
    QJsonObject root = tokensObj;

    QJsonObject doc;
    doc.insert("schemaVersion", 1);
    doc.insert("name",          newThemeName);
    doc.insert("author",        QStringLiteral("AetherSDR user"));
    doc.insert("version",       QStringLiteral("1.0"));
    doc.insert("description",   QStringLiteral("Created via the Theme Editor."));
    doc.insert("tokens",        root);

    // Mirror scanAvailableThemes — GenericConfigLocation + "/AetherSDR"
    // keeps the saved theme alongside the existing user-dir layout
    // (singular path, not the double-nested AppConfigLocation form).
    const QString userDir = QStandardPaths::writableLocation(
                                QStandardPaths::GenericConfigLocation)
                            + QStringLiteral("/AetherSDR/themes");
    QDir().mkpath(userDir);
    const QString path = userDir + QLatin1Char('/')
                       + newThemeName + QStringLiteral(".json");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcGui) << "ThemeManager: saveCurrentThemeAs failed to open"
                         << path << f.errorString();
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    f.close();

    // Register the new theme in the path map so availableThemes() picks it
    // up immediately, then make it the active theme (loads cleanly from
    // the file we just wrote, so the on-disk shape doubles as a validation
    // check).
    m_themePaths.insert(newThemeName, path);
    m_activeTheme = newThemeName;
    AppSettings::instance().setValue("ActiveTheme", newThemeName);
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
    // Gradient tokens: graceful fallback to the first stop's colour so
    // existing callers asking for a flat colour on what's now a gradient
    // token don't crash.  Callers that actually want the gradient should
    // use brush() or cssFragment().
    if (it.value().canConvert<ThemeGradient>()) {
        const auto g = it.value().value<ThemeGradient>();
        if (!g.stops.isEmpty()) return g.stops.first().color;
        return QColor(Qt::transparent);
    }
    return QColor(it.value().toString());
}

QBrush ThemeManager::brush(const QString& token, const QRect& bounds) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) {
        qCWarning(lcGui) << "ThemeManager: missing brush token" << token;
        return QBrush(Qt::transparent);
    }
    if (!it.value().canConvert<ThemeGradient>()) {
        // Scalar — wrap the colour into a solid brush.
        return QBrush(QColor(it.value().toString()));
    }
    const auto g = it.value().value<ThemeGradient>();
    if (g.type == ThemeGradient::Linear) {
        qreal nx1, ny1, nx2, ny2;
        linearAngleToEndpoints(g.angle, nx1, ny1, nx2, ny2);
        QPointF start, end;
        if (bounds.isValid()) {
            // Map normalised endpoints onto the requested rect.
            start = QPointF(bounds.x() + bounds.width()  * nx1,
                            bounds.y() + bounds.height() * ny1);
            end   = QPointF(bounds.x() + bounds.width()  * nx2,
                            bounds.y() + bounds.height() * ny2);
        } else {
            // Default to ObjectBoundingMode (0-1 normalised) — useful when
            // the caller intends to assign the brush via QPalette and let
            // Qt scale it to the widget at paint time.
            start = QPointF(nx1, ny1);
            end   = QPointF(nx2, ny2);
        }
        QLinearGradient lg(start, end);
        if (!bounds.isValid()) {
            lg.setCoordinateMode(QGradient::ObjectBoundingMode);
        }
        for (const auto& s : g.stops) lg.setColorAt(s.at, s.color);
        return QBrush(lg);
    }
    // Radial.
    QPointF center;
    qreal radius;
    if (bounds.isValid()) {
        center = QPointF(bounds.x() + bounds.width()  * g.center.x(),
                         bounds.y() + bounds.height() * g.center.y());
        radius = std::min(bounds.width(), bounds.height()) * g.radius;
    } else {
        center = g.center;
        radius = g.radius;
    }
    QRadialGradient rg(center, radius);
    if (!bounds.isValid()) {
        rg.setCoordinateMode(QGradient::ObjectBoundingMode);
    }
    for (const auto& s : g.stops) rg.setColorAt(s.at, s.color);
    return QBrush(rg);
}

QString ThemeManager::cssFragment(const QString& token) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.constEnd()) return QString();
    if (it.value().canConvert<ThemeGradient>()) {
        return gradientCssFragment(it.value().value<ThemeGradient>());
    }
    // Scalar tokens: route translucent values through rgba(...) so Qt's
    // stylesheet parser actually honours alpha.  Opaque values pass
    // through verbatim ("#rrggbb").
    return colorHexToCssFragment(it.value().toString());
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
    // Gradient tokens have no meaningful raw scalar — return empty so
    // callers that expected a string don't accidentally inline the
    // QVariant::toString() of a structured value.  Use cssFragment()
    // for the stylesheet form or brush() for paint code.
    if (it.value().canConvert<ThemeGradient>()) return QString();
    return it.value().toString();
}

QString ThemeManager::resolve(const QString& stylesheetTemplate) const
{
    // Replace every {{token.name}} with the token's stylesheet fragment.
    // Routes through cssFragment(): scalar colour tokens emit "#rrggbb",
    // numeric tokens emit their plain value ("12" — caller adds "px"),
    // gradient tokens emit qlineargradient(...) / qradialgradient(...).
    static const QRegularExpression kRe(QStringLiteral(R"(\{\{([^}]+)\}\})"));
    QString out = stylesheetTemplate;
    QRegularExpressionMatchIterator it = kRe.globalMatch(stylesheetTemplate);
    int offset = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString token = m.captured(1).trimmed();
        const QString val = cssFragment(token);
        // Adjust for the running offset as substitutions change length.
        out.replace(m.capturedStart(0) + offset,
                    m.capturedLength(0),
                    val);
        offset += (val.length() - m.capturedLength(0));
    }
    return out;
}

QStringList ThemeManager::extractReferencedTokens(const QString& stylesheetTemplate)
{
    static const QRegularExpression kRe(QStringLiteral(R"(\{\{([^}]+)\}\})"));
    QStringList tokens;
    QRegularExpressionMatchIterator it = kRe.globalMatch(stylesheetTemplate);
    while (it.hasNext()) {
        const QString token = it.next().captured(1).trimmed();
        if (!token.isEmpty() && !tokens.contains(token)) {
            tokens.append(token);
        }
    }
    return tokens;
}

void ThemeManager::applyStyleSheet(QWidget* widget, const QString& stylesheetTemplate)
{
    if (!widget) return;

    // Resolve and apply right away so the caller gets the same visual
    // result they'd have gotten from setStyleSheet(resolve(...)) — the
    // tracking is an additive side-effect, not a behaviour change.
    widget->setStyleSheet(resolve(stylesheetTemplate));

    // First-time registration: connect to destroyed() so the entry
    // disappears when the widget does.  Subsequent calls on the same
    // widget just overwrite the recorded template / token list.
    if (!m_trackedWidgets.contains(widget)) {
        connect(widget, &QObject::destroyed,
                this, &ThemeManager::onTrackedWidgetDestroyed);
    }
    TrackedWidget ctx;
    ctx.stylesheetTemplate = stylesheetTemplate;
    ctx.tokens = extractReferencedTokens(stylesheetTemplate);
    m_trackedWidgets.insert(widget, ctx);
}

void ThemeManager::clearWidgetTracking(QWidget* widget)
{
    if (!widget) return;
    if (m_trackedWidgets.remove(widget) > 0) {
        QObject::disconnect(widget, &QObject::destroyed,
                            this, &ThemeManager::onTrackedWidgetDestroyed);
    }
}

void ThemeManager::declareWidgetTokens(QWidget* widget, const QStringList& tokens)
{
    if (!widget) return;

    // Hook the destroyed() signal exactly once per widget — same convention
    // as applyStyleSheet so the inspector reverse-map never holds a
    // dangling pointer.
    if (!m_trackedWidgets.contains(widget)) {
        connect(widget, &QObject::destroyed,
                this, &ThemeManager::onTrackedWidgetDestroyed);
    }
    // Preserve any regions previously declared for the widget — token
    // updates and region updates are independent facets.
    TrackedWidget ctx = m_trackedWidgets.value(widget);
    ctx.stylesheetTemplate.clear();
    ctx.tokens = tokens;
    m_trackedWidgets.insert(widget, ctx);
}

void ThemeManager::declareWidgetRegions(QWidget* widget,
                                        const QList<ThemeRegion>& regions)
{
    if (!widget) return;

    if (!m_trackedWidgets.contains(widget)) {
        connect(widget, &QObject::destroyed,
                this, &ThemeManager::onTrackedWidgetDestroyed);
    }
    TrackedWidget ctx = m_trackedWidgets.value(widget);
    ctx.regions = regions;
    // Mirror each region's token into the coarse token list so
    // tokensForWidget() (the fallback path) still returns sensible
    // results when the click point misses every hit-test.
    QStringList rt;
    for (const auto& r : regions) {
        if (!r.token.isEmpty() && !rt.contains(r.token))
            rt.append(r.token);
    }
    if (ctx.tokens.isEmpty()) ctx.tokens = rt;
    m_trackedWidgets.insert(widget, ctx);
}

QStringList ThemeManager::tokensAtPoint(const QWidget* widget,
                                       const QPoint& localPos) const
{
    if (!widget) return QStringList();
    const auto it = m_trackedWidgets.constFind(const_cast<QWidget*>(widget));
    if (it == m_trackedWidgets.constEnd()) return QStringList();

    const auto& ctx = it.value();
    QStringList hits;
    for (const auto& r : ctx.regions) {
        if (r.hitTest && r.hitTest(localPos) && !hits.contains(r.token)) {
            hits.append(r.token);
        }
    }
    if (!hits.isEmpty()) return hits;
    // No region claimed the point — fall back to the coarse token list
    // so the inspector still surfaces something useful for the widget.
    return ctx.tokens;
}

QStringList ThemeManager::tokensForWidget(const QWidget* widget) const
{
    // const_cast is safe — the QHash lookup doesn't mutate the widget,
    // we just need a non-const key to match the storage type used by
    // applyStyleSheet().
    const auto it = m_trackedWidgets.constFind(const_cast<QWidget*>(widget));
    if (it == m_trackedWidgets.constEnd()) return QStringList();
    return it.value().tokens;
}

void ThemeManager::onTrackedWidgetDestroyed(QObject* obj)
{
    // The sender is the QWidget being destroyed.  By the time the
    // destroyed() signal fires, ~QWidget has already run; obj is a
    // QObject* in its destruction phase — safe to use as a hash key
    // (we just need its address for lookup) but we can NOT call any
    // QWidget methods on it.
    m_trackedWidgets.remove(static_cast<QWidget*>(obj));
}

void ThemeManager::reapplyAllTrackedStyleSheets()
{
    // Snapshot the keys before iterating — re-applying a stylesheet can
    // trigger arbitrary widget code (lazy-builds, focus shifts, etc.)
    // that might in turn touch m_trackedWidgets.  Iterating a copy
    // avoids the QHash-mutation-during-iteration undefined behaviour.
    const auto widgets = m_trackedWidgets.keys();
    for (QWidget* w : widgets) {
        const auto it = m_trackedWidgets.constFind(w);
        if (it == m_trackedWidgets.constEnd()) continue;  // dropped mid-sweep
        // Paint-code widgets registered via declareWidgetTokens() have an
        // empty template — skip them so we don't wipe any stylesheet they
        // may have inherited from a parent / Theme.h helper.  They handle
        // theme changes by connecting to themeChanged themselves.
        const QString& tmpl = it.value().stylesheetTemplate;
        if (tmpl.isEmpty()) continue;
        w->setStyleSheet(resolve(tmpl));
    }
}

} // namespace AetherSDR
