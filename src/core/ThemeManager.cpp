#include "ThemeManager.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QEvent>
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
        } else if (obj.contains("centerX") || obj.contains("centerY")) {
            // Recovery path for themes written before the writer was fixed: it
            // emitted centerX/centerY as two scalars, which this parser never
            // looked for, so those files load with a lost centre. Accept the
            // old shape on read so an existing user theme keeps its gradient;
            // the next save rewrites it as the "center" array.
            g.center = QPointF(obj.value("centerX").toDouble(0.5),
                               obj.value("centerY").toDouble(0.5));
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

// Parse a JSON compound-font object into the structured ThemeFont form.
// Schema: { "family": "Inter", "size": 12, "color": "#c8d8e8" }
// Family is required; size defaults to 0 (caller uses role-default);
// color defaults to invalid (caller falls back to color.text.primary).
ThemeFont parseFont(const QJsonObject& obj)
{
    ThemeFont f;
    f.family = obj.value("family").toString();
    if (obj.contains("size")) f.size = obj.value("size").toInt(0);
    if (obj.contains("color")) {
        const QColor c(obj.value("color").toString());
        if (c.isValid()) f.color = c;
    }
    return f;
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

// The single writer for the gradient JSON shape, and the exact inverse of
// parseGradient() above.  Both storage tiers route through here — semantic
// tokens via scopeToJson(), the primitives palette via themeDocumentJson() —
// because they used to hold one hand-rolled copy each and the copies drifted.
// The palette's copy emitted type/angle/stops only, so a radial gradient
// stored as a primitive lost its centre AND radius on every save: the same
// defect as the scope-level one, a tier down, and one parseGradient()'s
// centerX/centerY recovery cannot help with because there is no centerX in
// the file to recover from either.  One writer means the next shape fix can
// only ever need applying once.
QJsonObject gradientToJson(const ThemeGradient& g)
{
    QJsonObject gj;
    gj.insert("type", g.type == ThemeGradient::Radial
                        ? QStringLiteral("radial-gradient")
                        : QStringLiteral("linear-gradient"));
    gj.insert("angle", g.angle);
    if (g.type == ThemeGradient::Radial) {
        // "center" as a [x, y] ARRAY — the shape the reader parses and this
        // file's own format comment documents.  This used to write
        // centerX/centerY as two scalars, which the reader never looks for,
        // so a radial gradient came back at the {0.5, 0.5} default.
        // `radius` round-tripped fine, which made the loss look like a
        // half-working feature rather than a format mismatch.
        gj.insert("center", QJsonArray{g.center.x(), g.center.y()});
        gj.insert("radius", g.radius);
    }
    QJsonArray stops;
    for (const auto& s : g.stops) {
        QJsonObject sj;
        sj.insert("at",    s.at);
        sj.insert("color", colorToTokenString(s.color));
        stops.append(sj);
    }
    gj.insert("stops", stops);
    return gj;
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
            // Compound font object — required `family` field with no
            // `type` discriminator.  Tells this layer apart from the
            // legacy bare-string family tokens.
            if (inner.contains("family") && inner.value("family").isString()) {
                out.insert(key, QVariant::fromValue(parseFont(inner)));
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

// ─────────────────────────────────────────────── scope tree ──────────────

// A single node in the container scope tree.  Lives in the .cpp so the
// header doesn't need to expose its layout — public scope-aware API
// references scopes only by string path.
struct ThemeScope {
    QString name;                    // segment name (e.g. "spectrum")
    QString path;                    // full path "" / "spectrum" / "spectrum/panadapter"
    ThemeScope* parent {nullptr};
    QHash<QString, QVariant> tokens; // semantic tokens overridden at this scope
    // Children kept in a std::map (move-only-friendly + deterministic
    // iteration order) — QHash refuses unique_ptr values because its
    // implicit-copy paths can't compile against move-only types.
    std::map<QString, std::unique_ptr<ThemeScope>> children;
};

ThemeManager::~ThemeManager() = default;

ThemeScope* ThemeManager::scopeForPath(const QString& path) const
{
    if (path.isEmpty() || path == QLatin1String("root")) {
        return m_rootScope.get();
    }
    const auto it = m_scopeByPath.constFind(path);
    return (it == m_scopeByPath.constEnd()) ? nullptr : it.value();
}

ThemeScope* ThemeManager::scopeOrCreate(const QString& path)
{
    if (path.isEmpty() || path == QLatin1String("root")) return m_rootScope.get();
    if (auto* existing = scopeForPath(path)) return existing;

    // Walk segments from root, creating any missing intermediate scopes.
    ThemeScope* cur = m_rootScope.get();
    const QStringList segs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString runningPath;
    for (const QString& seg : segs) {
        runningPath = runningPath.isEmpty()
                          ? seg : runningPath + QLatin1Char('/') + seg;
        auto it = cur->children.find(seg);
        if (it == cur->children.end()) {
            auto child = std::make_unique<ThemeScope>();
            child->name   = seg;
            child->path   = runningPath;
            child->parent = cur;
            ThemeScope* raw = child.get();
            cur->children.emplace(seg, std::move(child));
            m_scopeByPath.insert(runningPath, raw);
            cur = raw;
        } else {
            cur = it->second.get();
        }
    }
    return cur;
}

QVariant ThemeManager::resolveAlias(const QVariant& v) const
{
    if (v.userType() != QMetaType::QString) return v;
    const QString s = v.toString();
    if (s.size() < 3 || !s.startsWith(QLatin1Char('{')) || !s.endsWith(QLatin1Char('}'))) {
        return v;
    }
    const QString key = s.mid(1, s.size() - 2);
    const auto it = m_primitives.constFind(key);
    if (it == m_primitives.constEnd()) return v;  // unresolved alias — return literal
    return it.value();
}

QVariant ThemeManager::lookupRaw(const QString& containerPath, const QString& key) const
{
    const ThemeScope* scope = scopeForPath(containerPath);
    if (!scope) scope = m_rootScope.get();
    while (scope) {
        const auto it = scope->tokens.constFind(key);
        if (it != scope->tokens.constEnd()) return resolveAlias(it.value());
        scope = scope->parent;
    }
    return {};
}

void ThemeManager::rebuildScopePathIndex()
{
    m_scopeByPath.clear();
    if (!m_rootScope) return;
    m_scopeByPath.insert(QString(), m_rootScope.get());      // empty key = root
    m_scopeByPath.insert(QStringLiteral("root"), m_rootScope.get());
    std::function<void(ThemeScope*)> walk = [&](ThemeScope* s) {
        for (auto& kv : s->children) {
            ThemeScope* child = kv.second.get();
            m_scopeByPath.insert(child->path, child);
            walk(child);
        }
    };
    walk(m_rootScope.get());
}

ThemeManager& ThemeManager::instance()
{
    static ThemeManager s_instance;
    return s_instance;
}

ThemeManager::ThemeManager()
    : m_rootScope(std::make_unique<ThemeScope>())
    , m_tokens(m_rootScope->tokens)
{
    // Root scope identity — empty name, empty path, no parent.  Indexed
    // under both "" (canonical) and "root" so callers using either form
    // resolve to the same node.
    m_rootScope->name = QString();
    m_rootScope->path = QString();
    m_scopeByPath.insert(QString(),                m_rootScope.get());
    m_scopeByPath.insert(QStringLiteral("root"),   m_rootScope.get());

    // Explicit metatype registration so qMetaTypeId<ThemeGradient>() and
    // direct userType comparisons resolve correctly even before the first
    // QVariant::fromValue<ThemeGradient>() call.  Q_DECLARE_METATYPE in
    // the header sets up the template machinery, but explicit registration
    // here makes saveCurrentThemeAs()'s type check timing-independent.
    qRegisterMetaType<ThemeGradient>("AetherSDR::ThemeGradient");
    qRegisterMetaType<ThemeFont>("AetherSDR::ThemeFont");

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
        // via the editor and later removed the file out-of-band).  Fall
        // back to "Default Dark" explicitly rather than limping along on
        // the seed: the seed is Default Dark's token set, so a user whose
        // saved theme was light-derived would silently get a dark UI with
        // no indication why.  (Before the seed was generated the reason
        // was starker — it had no waterfall.colormap gradients at all, so
        // the operator got a baffling all-grayscale waterfall.  #3184
        // fixed that; the fallback is still right for the theme-identity
        // reason.)
        qCWarning(lcGui) << "ThemeManager: saved theme" << saved
                          << "is unavailable — falling back to Default Dark";
        if (!setActiveTheme(QStringLiteral("Default Dark"))) {
            qCWarning(lcGui) << "ThemeManager: Default Dark also failed to load"
                              << "— UI will render with compiled-in defaults"
                              << "(rebuild resources?)";
        }
    }
}

void ThemeManager::seedScopedToken(const QString& containerPath,
                                   const QString& token, const QVariant& value)
{
    if (ThemeScope* s = scopeOrCreate(containerPath))
        s->tokens.insert(token, value);
}

void ThemeManager::seedBuiltinDefaults()
{
    // Compiled-in defaults, so the UI is usable with zero theme files on disk.
    //
    // The table is GENERATED from resources/themes/default-dark.json — see
    // src/core/ThemeSeedGenerated.cpp and tools/gen_theme_seed.py. It used to be
    // maintained by hand, and both failure modes the old comment warned about
    // had already happened (#3184): 9 tokens had drifted from the JSON, and 24
    // more were never seeded at all, resolving TRANSPARENT on any theme that
    // predated them.
    //
    // Regenerate with `python tools/gen_theme_seed.py` after editing the bundled
    // theme; tools/check_theme_seed.py --strict fails the build's PR gate when
    // the two disagree.  Do NOT hand-add tokens here — that is the dual source
    // of truth this replaced.  tests/theme_seed_test.cpp pins the seed values
    // that actually reach m_tokens.
    seedGeneratedDefaults();
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

    // Reset the scope tree before loading the new theme.  Compiled-in
    // defaults stay as the fallback layer so older theme files with
    // fewer tokens still produce a fully-rendered UI on a newer build.
    //
    // Ordering matters: clear() empties any child scopes from the
    // previous theme load (including the per-applet scope seeds), then
    // seedBuiltinDefaults() re-seeds them via scopeOrCreate().  If the
    // active theme's JSON contains its own nested scopes, readScopeFromJson
    // below overwrites the seeded values where they overlap — the seeds
    // are the floor, the JSON wins where defined.
    m_rootScope->children.clear();
    m_primitives.clear();
    // Per-theme, not per-process: a token missing only in THIS theme should be
    // reported again when the operator comes back to it, otherwise the one log
    // line they need has scrolled away and never reappears.
    {
        QMutexLocker lock(&m_unknownTokenMutex);
        m_warnedUnknownTokens.clear();
    }
    rebuildScopePathIndex();
    seedBuiltinDefaults();  // resets m_tokens (= root scope tokens) + re-seeds applet/* scope tree

    if (schemaVersion >= 2) {
        // v2 — primitives + nested scopes.  Canonical shape is
        // `scopes: { root: { tokens, scopes } }`; we unwrap the literal
        // "root" here so readScopeFromJson only sees the recursive
        // {tokens, scopes} shape.
        readPrimitivesFromJson(root.value("primitives").toObject());
        if (root.contains("scopes")) {
            const QJsonObject scopesObj = root.value("scopes").toObject();
            if (scopesObj.contains("root")) {
                readScopeFromJson(scopesObj.value("root").toObject(),
                                  m_rootScope.get());
            } else {
                // Tolerate files that drop the root wrapper — every
                // entry under "scopes" becomes a direct child of root.
                readScopeFromJson(QJsonObject{{"scopes", scopesObj}},
                                  m_rootScope.get());
            }
        } else if (root.contains("tokens")) {
            // Tolerated mixed shape: v2 file with no scope wrapper —
            // tokens at the top level land in root scope.
            flattenTokens(root.value("tokens").toObject(),
                          QString(), m_rootScope->tokens);
        }
    } else {
        // v1 — flat tokens, all in root scope.  Auto-migrated to v2 on
        // the next saveActiveTheme().
        flattenTokens(root.value("tokens").toObject(),
                      QString(), m_rootScope->tokens);
    }
    rebuildScopePathIndex();
    // Replay declared containers — the JSON load just wiped the scope
    // tree's children, but widgets registered their container paths
    // at construction time and we need those scopes to keep existing
    // so the editor's tree picker can still navigate to them.
    for (const QString& path : m_declaredContainers) scopeOrCreate(path);

    // Decide which bundled theme this one counts as a fork of, ONCE, here —
    // with the file's own values loaded and no operator edits applied yet.
    //
    // This has to be a load-time decision rather than a lookup at Reset time.
    // The fallback discriminator is background luminance, and the operator
    // reaches for "Reset to default" precisely when they have just made a
    // value wrong: classify on the live token and a light theme whose
    // background has been dragged dark reclassifies as dark, so Reset restores
    // the DARK value for that token and every other one for the rest of the
    // session — the exact defect this is meant to fix, self-inflicted.
    m_activeThemeBase = resolveThemeBase(root);
    return true;
}

QString ThemeManager::resolveThemeBase(const QJsonObject& root) const
{
    // Prefer recorded parentage. saveCurrentThemeAs() stamps the base it forked
    // from into the document, so for any theme this build wrote there is no
    // guessing at all.
    const QString recorded = root.value(QStringLiteral("baseTheme")).toString();
    if (recorded == QLatin1String("Default Light")
        || recorded == QLatin1String("Default Dark")) {
        return recorded;
    }
    // No parentage recorded (a bundled theme, a hand-written file, or one
    // saved by a build predating the field): fall back to the theme's own
    // background luminance as loaded from disk.
    const QVariant bg = lookupRaw(QString(),
                                  QStringLiteral("color.background.0"));
    const QColor c(bg.toString());
    if (c.isValid() && c.lightness() > 127) return QStringLiteral("Default Light");
    return QStringLiteral("Default Dark");
}

void ThemeManager::readPrimitivesFromJson(const QJsonObject& obj)
{
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QJsonValue v = it.value();
        if (v.isString())      m_primitives.insert(it.key(), v.toString());
        else if (v.isDouble()) m_primitives.insert(it.key(), v.toDouble());
        else if (v.isBool())   m_primitives.insert(it.key(), v.toBool());
        else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.contains("type") && o.value("type").isString()) {
                m_primitives.insert(it.key(),
                                    QVariant::fromValue(parseGradient(o)));
            }
        }
    }
}

void ThemeManager::readScopeFromJson(const QJsonObject& obj, ThemeScope* into)
{
    // Per-scope shape: `{tokens: {...}, scopes: {<name>: <subscope>}}`.
    // The loader is responsible for unwrapping the outermost "root"
    // before calling here; this function handles the recursive shape.
    if (obj.contains("tokens")) {
        flattenTokens(obj.value("tokens").toObject(),
                      QString(), into->tokens);
    }
    if (obj.contains("scopes")) {
        const QJsonObject children = obj.value("scopes").toObject();
        for (auto it = children.constBegin(); it != children.constEnd(); ++it) {
            const QString name = it.key();
            ThemeScope* childScope = nullptr;
            auto existing = into->children.find(name);
            if (existing != into->children.end()) {
                childScope = existing->second.get();
            } else {
                auto child = std::make_unique<ThemeScope>();
                child->name = name;
                child->path = into->path.isEmpty()
                                  ? name
                                  : into->path + QLatin1Char('/') + name;
                child->parent = into;
                childScope = child.get();
                into->children.emplace(name, std::move(child));
            }
            readScopeFromJson(it.value().toObject(), childScope);
        }
    }
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
    // Smart-invalidation hint scope — reapplyAllTrackedStyleSheets reads
    // this during the synchronous themeChanged dispatch and skips every
    // tracked widget whose template doesn't reference `token`.  Cleared
    // before returning so subsequent full-theme reloads (setActiveTheme)
    // walk every widget.
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

void ThemeManager::setSizing(const QString& token, int value)
{
    const auto it = m_tokens.constFind(token);
    if (it != m_tokens.constEnd() && it.value().toInt() == value) return;
    m_tokens.insert(token, QVariant(value));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

ThemeGradient ThemeManager::gradient(const QString& token) const
{
    const QVariant v = lookupRaw(QString(), token);
    if (v.userType() != qMetaTypeId<ThemeGradient>()) return {};
    return v.value<ThemeGradient>();
}

void ThemeManager::setGradient(const QString& token, const ThemeGradient& g)
{
    m_tokens.insert(token, QVariant::fromValue(g));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

void ThemeManager::setString(const QString& token, const QString& value)
{
    const auto it = m_tokens.constFind(token);
    if (it != m_tokens.constEnd() &&
        it.value().userType() == QMetaType::QString &&
        it.value().toString() == value) {
        return;
    }
    m_tokens.insert(token, QVariant(value));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

bool ThemeManager::saveActiveTheme()
{
    if (m_activeTheme.isEmpty()) return false;
    if (isBuiltInTheme(m_activeTheme)) {
        // Built-ins live in :/themes/ and can't be overwritten.  The
        // TokenEditorWidget gates OK clicks on built-in themes through a
        // Save As prompt, so the only way we'd land here is a setter
        // being called outside that flow — be defensive and skip.
        return false;
    }
    const auto it = m_themePaths.constFind(m_activeTheme);
    if (it == m_themePaths.constEnd()) return false;
    return writeThemeFile(m_activeTheme, it.value());
}

QString ThemeManager::factoryBaselinePath() const
{
    // Which bundled theme "factory default" means depends on what the operator
    // is editing. This used to be hardcoded to default-dark.json, so pressing
    // "Reset to default" while editing Default Light restored the DARK value —
    // wrong for most of the root tokens the two themes share, including
    // color.background.0, which flipped a near-white background to near-black.
    //
    // m_activeThemeBase is decided once per theme load (see resolveThemeBase),
    // from recorded parentage where we have it and from the file's own
    // background luminance where we don't. Deliberately NOT recomputed here:
    // this is read from the Reset button, and the operator presses Reset
    // exactly when a value is wrong — classifying on live state lets a
    // dragged-dark background on a light theme flip the whole baseline.
    return m_activeThemeBase == QLatin1String("Default Light")
               ? QStringLiteral(":/themes/default-light.json")
               : QStringLiteral(":/themes/default-dark.json");
}

void ThemeManager::ensureFactoryLoaded() const
{
    // Keyed on the baseline the ACTIVE theme resolves to, not loaded once for
    // the process: switching Dark -> Light has to re-snapshot, or the editor
    // keeps offering the previous base's values.
    const QString wanted = factoryBaselinePath();
    if (m_factoryLoaded && m_factoryBaselinePath == wanted) return;
    m_factoryTokens.clear();  // stale entries from the other base must not leak

    QFile f(wanted);
    if (!f.open(QIODevice::ReadOnly)) {
        // Do NOT latch on failure. The old code set m_factoryLoaded before the
        // load could fail, so a single miss disabled every Reset affordance for
        // the rest of the process with nothing in the UI to say why. Leaving
        // the flag clear costs one failed QFile::open per Reset press and lets
        // it recover if the resource ever becomes readable.
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
    const QJsonObject root = doc.object();
    const int schemaVer = root.value("schemaVersion").toInt(0);
    if (schemaVer >= 2) {
        // v2: tokens live under scopes.root.tokens, and most values are
        // {primitive} aliases that need resolving before factoryColor()
        // can return a valid QColor.  The legacy single-line flattenTokens
        // call below worked for v1 (flat tokens at the document root) but
        // landed an empty m_factoryTokens against every v2 theme — which
        // silently disabled the Reset button at root scope across the
        // whole editor.
        QHash<QString, QString> factoryPrims;
        const QJsonObject primitives = root.value("primitives").toObject();
        for (auto it = primitives.constBegin(); it != primitives.constEnd(); ++it) {
            if (it.value().isString())
                factoryPrims.insert(it.key(), it.value().toString());
        }
        const QJsonObject scopesObj = root.value("scopes").toObject();
        const QJsonObject rootScope = scopesObj.value("root").toObject();
        flattenTokens(rootScope.value("tokens").toObject(), QString(),
                      m_factoryTokens);
        // Resolve `{primitive}` aliases inline so factoryColor() /
        // factoryString() see concrete hex / family values without
        // needing a second lookup pass.  Single-hop resolution matches
        // the live runtime resolver (resolveAlias in this file).
        for (auto it = m_factoryTokens.begin(); it != m_factoryTokens.end(); ++it) {
            if (it.value().userType() != QMetaType::QString) continue;
            const QString s = it.value().toString();
            if (s.size() < 3 ||
                !s.startsWith(QLatin1Char('{')) ||
                !s.endsWith(QLatin1Char('}'))) continue;
            const auto pit = factoryPrims.constFind(s.mid(1, s.size() - 2));
            if (pit != factoryPrims.constEnd()) it.value() = pit.value();
        }
    } else {
        // v1: flat tokens at document root.  Auto-migrated to v2 on
        // saveActiveTheme, but pre-migration files still load through
        // this branch.
        flattenTokens(root.value("tokens").toObject(), QString(),
                      m_factoryTokens);
    }
    // Latch only on success — every early return above leaves the flags clear
    // so the next Reset press retries instead of serving an empty snapshot.
    m_factoryLoaded       = true;
    m_factoryBaselinePath = wanted;
}

ThemeGradient ThemeManager::factoryGradient(const QString& token) const
{
    ensureFactoryLoaded();
    const auto it = m_factoryTokens.constFind(token);
    if (it == m_factoryTokens.constEnd()) return {};
    if (!it.value().canConvert<ThemeGradient>()) return {};
    return it.value().value<ThemeGradient>();
}

QColor ThemeManager::factoryColor(const QString& token) const
{
    ensureFactoryLoaded();
    const auto it = m_factoryTokens.constFind(token);
    if (it == m_factoryTokens.constEnd()) return QColor();
    if (it.value().canConvert<ThemeGradient>()) {
        // Token is a gradient in the factory baseline — graceful fallback
        // to the first stop's colour, matching ThemeManager::color()'s
        // behaviour for gradient tokens.
        const auto g = it.value().value<ThemeGradient>();
        if (g.stops.isEmpty()) return QColor();
        return g.stops.first().color;
    }
    if (it.value().userType() != QMetaType::QString) return QColor();
    return QColor(it.value().toString());
}

int ThemeManager::factorySizing(const QString& token) const
{
    ensureFactoryLoaded();
    const auto it = m_factoryTokens.constFind(token);
    if (it == m_factoryTokens.constEnd()) return -1;
    bool ok = false;
    const int v = it.value().toInt(&ok);
    if (!ok) return -1;
    return v;
}

QString ThemeManager::factoryString(const QString& token) const
{
    ensureFactoryLoaded();
    const auto it = m_factoryTokens.constFind(token);
    if (it == m_factoryTokens.constEnd()) return QString();
    if (it.value().userType() != QMetaType::QString) return QString();
    return it.value().toString();
}

bool ThemeManager::hasFactoryValue(const QString& token) const
{
    ensureFactoryLoaded();
    return m_factoryTokens.contains(token);
}

bool ThemeManager::isBuiltInTheme(const QString& name) const
{
    // Built-ins live inside the Qt resource bundle.  Path map records
    // ":/themes/<file>.json" for them; user themes use absolute paths.
    const auto it = m_themePaths.constFind(name);
    if (it == m_themePaths.constEnd()) return false;
    return it.value().startsWith(QStringLiteral(":/themes/"));
}

bool ThemeManager::deleteTheme(const QString& name)
{
    if (name.isEmpty()) return false;
    if (isBuiltInTheme(name)) {
        qCWarning(lcGui) << "ThemeManager::deleteTheme: refusing to delete "
                            "built-in theme" << name;
        return false;
    }
    const auto it = m_themePaths.constFind(name);
    if (it == m_themePaths.constEnd()) return false;
    const QString path = it.value();

    // If the operator is deleting the currently active theme, fall back
    // to Default Dark before unlinking — otherwise every consumer of the
    // active theme would briefly render against an inconsistent token
    // map while the file is still being closed.
    if (m_activeTheme == name) {
        setActiveTheme(QStringLiteral("Default Dark"));
    }

    QFile f(path);
    if (!f.remove()) {
        qCWarning(lcGui) << "ThemeManager::deleteTheme: unlink failed for"
                         << path << f.errorString();
        return false;
    }
    m_themePaths.remove(name);
    return true;
}

// A theme name that becomes a filename must not carry path structure — and,
// because a theme file is portable, must not be a name that only breaks once
// the file reaches another OS.
//
// Checked rather than silently rewritten at the two call sites where the
// operator TYPED the name: replacing characters would save their theme under a
// name they did not choose. importThemeFromFile() substitutes instead, and that
// is right for it — there the name comes from a file's JSON, nobody is
// watching, and refusing would strand an otherwise-valid import.
//
// Every rule below is applied on every platform, deliberately. '\' and ':' are
// not special on Unix and the reserved device names mean nothing there, but a
// theme saved on Linux gets opened on Windows, and the failure then belongs to
// someone who never typed anything.
//
// `reason` is filled with operator-facing text — the caller is a dialog, and
// "we refused, here is why" is the entire justification for refusing rather
// than substituting. Silently returning false and letting the UI guess is how
// you end up telling someone to check directory permissions.
bool ThemeManager::isValidThemeName(const QString& name, QString* reason)
{
    auto no = [reason](const QString& why) {
        if (reason) *reason = why;
        return false;
    };
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return no(QStringLiteral("A theme name can't be empty."));

    if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\')))
        return no(QStringLiteral("A theme name can't contain \"/\" or \"\\\" — "
                                 "it becomes a filename."));
    if (trimmed.contains(QLatin1Char(':')))
        return no(QStringLiteral("A theme name can't contain \":\" — it becomes "
                                 "a filename, and \":\" is reserved on Windows."));
    for (const QChar c : trimmed) {
        if (c.unicode() < 0x20)
            return no(QStringLiteral("A theme name can't contain control "
                                     "characters."));
    }
    // Win32 strips a trailing dot, so "My Theme." and "My Theme" would collide
    // on disk while reading as two different themes in the list.
    if (trimmed.endsWith(QLatin1Char('.')))
        return no(QStringLiteral("A theme name can't end with \".\"."));

    // Reserved DOS device names: still special on Win32 today, with or without
    // an extension, so "CON.json" opens the console rather than a file.
    static const QStringList kReserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    const QString stem = trimmed.section(QLatin1Char('.'), 0, 0).toUpper();
    if (kReserved.contains(stem))
        return no(QStringLiteral("\"%1\" is a name Windows reserves for a "
                                 "device — pick another.").arg(trimmed));

    if (reason) reason->clear();
    return true;
}

bool ThemeManager::renameTheme(const QString& oldName, const QString& newName)
{
    const QString trimmed = newName.trimmed();
    if (oldName.isEmpty() || trimmed.isEmpty()) return false;
    if (oldName == trimmed) return true;  // no-op rename
    // Becomes a filename below — same rule as saveCurrentThemeAs() and
    // importThemeFromFile().
    QString why;
    if (!isValidThemeName(trimmed, &why)) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: refusing name"
                         << trimmed << "—" << why;
        return false;
    }
    if (isBuiltInTheme(oldName)) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: refusing to rename "
                            "built-in theme" << oldName;
        return false;
    }
    if (m_themePaths.contains(trimmed)) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: target already exists"
                         << trimmed;
        return false;
    }
    const auto it = m_themePaths.constFind(oldName);
    if (it == m_themePaths.constEnd()) return false;
    const QString oldPath = it.value();
    const QFileInfo fi(oldPath);
    const QString newPath = fi.absolutePath()
                            + QLatin1Char('/') + trimmed + QStringLiteral(".json");

    // The theme name is stored inside the JSON too — round-trip the file
    // through QJsonDocument so the on-disk "name" field matches the new
    // filename (otherwise the editor's "Editing: …" header would lie).
    QFile f(oldPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: cannot read"
                         << oldPath << f.errorString();
        return false;
    }
    QJsonParseError perr;
    auto doc = QJsonDocument::fromJson(f.readAll(), &perr);
    f.close();
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: parse error"
                         << perr.errorString();
        return false;
    }
    QJsonObject root = doc.object();
    root.insert("name", trimmed);
    doc.setObject(root);

    QFile out(newPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcGui) << "ThemeManager::renameTheme: cannot write"
                         << newPath << out.errorString();
        return false;
    }
    out.write(doc.toJson(QJsonDocument::Indented));
    out.close();

    QFile::remove(oldPath);
    m_themePaths.remove(oldName);
    m_themePaths.insert(trimmed, newPath);

    if (m_activeTheme == oldName) {
        m_activeTheme = trimmed;
        AppSettings::instance().setValue("ActiveTheme", trimmed);
        AppSettings::instance().save();
        emit themeChanged();
    }
    return true;
}

QJsonObject ThemeManager::scopeToJson(const ThemeScope* scope) const
{
    // Per-scope shape: { "tokens": {...}, "scopes": {<child>: ...} }.
    // Either / both may be omitted when empty.  Tokens are written flat
    // (dotted keys) — the migration through scope tree doesn't try to
    // re-nest token keys back into the legacy bundled layout, both
    // because earlier deep-walk attempts at that hit prefix-conflict
    // bugs (color.accent vs. color.accent.bright) and because the
    // editor never reads the file in nested form.
    const int gradMetaId = qMetaTypeId<ThemeGradient>();
    const int fontMetaId = qMetaTypeId<ThemeFont>();
    QJsonObject result;
    if (!scope->tokens.isEmpty()) {
        QJsonObject toks;
        for (auto it = scope->tokens.constBegin(); it != scope->tokens.constEnd(); ++it) {
            const QVariant& v = it.value();
            const int ut = v.userType();
            QJsonValue leaf;
            if (ut == QMetaType::QString)      leaf = v.toString();
            else if (ut == QMetaType::Int)     leaf = v.toInt();
            else if (ut == QMetaType::Double)  leaf = v.toDouble();
            else if (ut == QMetaType::Bool)    leaf = v.toBool();
            else if (ut == gradMetaId) {
                leaf = gradientToJson(v.value<ThemeGradient>());
            }
            else if (ut == fontMetaId) {
                const ThemeFont f = v.value<ThemeFont>();
                QJsonObject fj;
                fj.insert("family", f.family);
                if (f.size > 0)       fj.insert("size",  f.size);
                if (f.color.isValid()) fj.insert("color", colorToTokenString(f.color));
                leaf = fj;
            }
            else {
                qCWarning(lcGui) << "ThemeManager::scopeToJson: skipping token"
                                 << it.key() << "with unsupported metatype" << ut;
                continue;
            }
            toks.insert(it.key(), leaf);
        }
        result.insert("tokens", toks);
    }
    if (!scope->children.empty()) {
        QJsonObject scopesObj;
        for (auto& kv : scope->children) {
            scopesObj.insert(kv.first, scopeToJson(kv.second.get()));
        }
        result.insert("scopes", scopesObj);
    }
    return result;
}

QJsonObject ThemeManager::themeDocumentJson(const QString& themeName,
                                            const QString& description) const
{
    // THE one place a theme document is assembled.  Save and export both come
    // through here, so they cannot disagree about what a theme file contains —
    // which they did: export hand-rolled its own document and left out
    // `primitives`, and scope tokens store `{color.red.500}` aliases verbatim,
    // so an exported theme carried aliases pointing into a palette that wasn't
    // in the file.  On import resolveAlias() returned the literal and QColor
    // got handed "{color.red.500}".
    //
    // v2 schema — primitives map + nested scope tree.  v1 themes loaded from
    // disk auto-upgrade on first save through this writer.
    QJsonObject primitives;
    const int gradMetaId = qMetaTypeId<ThemeGradient>();
    for (auto it = m_primitives.constBegin(); it != m_primitives.constEnd(); ++it) {
        const QVariant& v = it.value();
        const int ut = v.userType();
        if (ut == QMetaType::QString)      primitives.insert(it.key(), v.toString());
        else if (ut == QMetaType::Int)     primitives.insert(it.key(), v.toInt());
        else if (ut == QMetaType::Double)  primitives.insert(it.key(), v.toDouble());
        else if (ut == QMetaType::Bool)    primitives.insert(it.key(), v.toBool());
        else if (ut == gradMetaId) {
            // Literally the same writer the scope tokens use — this used to
            // be a separate copy that omitted `center` and `radius`, so a
            // radial gradient in the palette round-tripped to the defaults.
            primitives.insert(it.key(), gradientToJson(v.value<ThemeGradient>()));
        }
    }

    QJsonObject scopes;
    scopes.insert(QStringLiteral("root"), scopeToJson(m_rootScope.get()));

    QJsonObject doc;
    doc.insert("schemaVersion", 2);
    doc.insert("name",          themeName);
    doc.insert("author",        QStringLiteral("AetherSDR user"));
    doc.insert("version",       QStringLiteral("1.0"));
    doc.insert("description",   description);
    // Recorded parentage: which bundled theme this one descends from, so
    // "Reset to default" restores the right base without having to guess from
    // the theme's own colours.  Additive within v2 — older builds ignore an
    // unknown key, and resolveThemeBase() falls back to luminance when it's
    // absent, which is every file written before this field existed.
    if (!m_activeThemeBase.isEmpty())
        doc.insert("baseTheme", m_activeThemeBase);
    if (!primitives.isEmpty()) doc.insert("primitives", primitives);
    doc.insert("scopes",        scopes);
    return doc;
}

bool ThemeManager::writeThemeFile(const QString& themeName, const QString& path)
{
    const QJsonObject doc = themeDocumentJson(
        themeName, QStringLiteral("Edited via the Theme Editor."));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcGui) << "ThemeManager::writeThemeFile failed to open"
                         << path << f.errorString();
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool ThemeManager::saveCurrentThemeAs(const QString& newThemeName)
{
    // Trim ONCE and use the trimmed name for everything below — the guard, the
    // filename, and the map key.  The old code trimmed only for the emptiness
    // test, so "My Theme " registered under a key with a trailing space and
    // wrote "My Theme .json"; Win32 strips trailing spaces from filenames, so
    // the key and the file on disk would disagree there.  renameTheme() has
    // always trimmed throughout; match it.
    const QString name = newThemeName.trimmed();
    // The name becomes a FILENAME below. importThemeFromFile() already refuses
    // path separators for exactly this reason ("sanitise so we can't
    // path-traverse via a malicious name field"), but this entry point — the
    // editor's Save As, where the operator types the name — did not, so a name
    // containing a separator wrote outside the themes directory. Same rule,
    // applied consistently.
    QString why;
    if (!isValidThemeName(name, &why)) {
        qCWarning(lcGui) << "ThemeManager::saveCurrentThemeAs: refusing name"
                         << newThemeName << "—" << why;
        return false;
    }

    // Mirror scanAvailableThemes — GenericConfigLocation + "/AetherSDR"
    // keeps the saved theme alongside the existing user-dir layout
    // (singular path, not the double-nested AppConfigLocation form).
    const QString userDir = QStandardPaths::writableLocation(
                                QStandardPaths::GenericConfigLocation)
                            + QStringLiteral("/AetherSDR/themes");
    QDir().mkpath(userDir);
    const QString path = userDir + QLatin1Char('/')
                       + name + QStringLiteral(".json");
    if (!writeThemeFile(name, path)) return false;

    // Register the new theme in the path map so availableThemes() picks it
    // up immediately, then make it the active theme (loads cleanly from
    // the file we just wrote, so the on-disk shape doubles as a validation
    // check).
    //
    // m_activeThemeBase is deliberately carried across unchanged: a fork of
    // Default Light is still a descendant of Default Light, and that is exactly
    // the parentage writeThemeFile() just stamped into the new file.
    m_themePaths.insert(name, path);
    m_activeTheme = name;
    AppSettings::instance().setValue("ActiveTheme", name);
    AppSettings::instance().save();
    emit themeChanged();
    return true;
}

bool ThemeManager::exportThemeToFile(const QString& themeName,
                                     const QString& filePath,
                                     QString* errorMessage) const
{
    auto fail = [errorMessage](const QString& msg) {
        if (errorMessage) *errorMessage = msg;
        return false;
    };
    if (themeName.isEmpty()) return fail(QStringLiteral("Empty theme name."));
    if (filePath.isEmpty())  return fail(QStringLiteral("Empty file path."));

    // For the *active* theme, the live scope tree already holds the operator's
    // session edits — saveCurrentThemeAs serialises the same thing.  For any
    // other theme, re-read its on-disk JSON so we don't accidentally export the
    // active theme's tokens under a different name.
    QJsonObject doc;
    if (themeName == m_activeTheme) {
        // Build the SAME document the save path writes, through the same
        // function, instead of hand-rolling one here.
        //
        // The old code walked m_tokens and wrote a top-level "tokens" object.
        // But m_tokens is a reference into the ROOT SCOPE only (see the header)
        // — every child scope lives in m_rootScope/m_scopeByPath and was simply
        // absent from the export. On the bundled dark theme that silently drops
        // 9 of 12 scoped tokens: all the per-applet slider/knob/toggle
        // overrides for applet/tx, applet/rx and applet/comp.
        //
        // The file still LOADED (the reader has a legacy flat-"tokens"
        // fallback), which is what made this invisible: the operator got a
        // theme file back that opened cleanly and had quietly lost its
        // per-applet differentiation.
        //
        // Sharing themeDocumentJson() rather than just scopeToJson() matters:
        // scope tokens store `{color.red.500}` ALIASES verbatim, so a document
        // carrying scopes without the `primitives` palette they point into is
        // worse than one carrying neither — resolveAlias() hands the literal
        // back and QColor("{color.red.500}") is invalid. Every one of the nine
        // scoped tokens on the bundled dark theme is such an alias.
        doc = themeDocumentJson(themeName,
                                QStringLiteral("Exported via the Theme Editor."));
    } else {
        const auto pit = m_themePaths.constFind(themeName);
        if (pit == m_themePaths.constEnd())
            return fail(QStringLiteral("Theme \"%1\" is not registered.").arg(themeName));
        QFile src(pit.value());
        if (!src.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Cannot read source theme: %1").arg(src.errorString()));
        QJsonParseError perr;
        const auto parsed = QJsonDocument::fromJson(src.readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !parsed.isObject())
            return fail(QStringLiteral("Source theme is not valid JSON: %1").arg(perr.errorString()));
        doc = parsed.object();
    }

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Cannot write file: %1").arg(f.errorString()));
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

QString ThemeManager::importThemeFromFile(const QString& filePath,
                                          QString* errorMessage)
{
    auto fail = [errorMessage](const QString& msg) -> QString {
        if (errorMessage) *errorMessage = msg;
        return QString();
    };
    if (filePath.isEmpty()) return fail(QStringLiteral("Empty file path."));

    QFile in(filePath);
    if (!in.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot open file: %1").arg(in.errorString()));
    QJsonParseError perr;
    const auto doc = QJsonDocument::fromJson(in.readAll(), &perr);
    in.close();
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("File is not a valid theme JSON: %1").arg(perr.errorString()));

    const QJsonObject root = doc.object();
    const int schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(0);
    if (schemaVersion <= 0)
        return fail(QStringLiteral("Missing or invalid schemaVersion — file may not be a theme."));
    if (schemaVersion > 2) {
        // Forward-compatible-ish: load anyway, but warn the operator.
        // Unknown tokens round-trip; missing tokens fall back to factory.
        //
        // The gate was `> 1`, which is stale: loadThemeFile() has read v2
        // natively since the scope-tree work, and both the save and export
        // paths now emit v2 — so every ordinary "export a theme, hand it to
        // another operator, import it" round trip logged "newer than this
        // build supports". v2 IS what this build writes.
        qCWarning(lcGui) << "ThemeManager::importThemeFromFile: schemaVersion"
                         << schemaVersion << "is newer than this build supports;"
                         << "loading anyway with unknown tokens preserved.";
    }

    // Pick a destination name: prefer the JSON's "name" field, fall back
    // to the file's basename.  Sanitise so we can't path-traverse via
    // a malicious name field — slashes get replaced with underscores.
    QString name = root.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) name = QFileInfo(filePath).completeBaseName();
    name.replace(QLatin1Char('/'),  QLatin1Char('_'));
    name.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (name.isEmpty()) return fail(QStringLiteral("Cannot derive a theme name from the file."));

    // Refuse to clobber a built-in theme by name — the user can't edit
    // those anyway and the resource-bundle copy shadows whatever lands
    // on disk, which would be confusing.
    if (isBuiltInTheme(name))
        return fail(QStringLiteral("\"%1\" matches a built-in theme name. "
                                   "Rename the file's \"name\" field or "
                                   "rename the file itself.").arg(name));

    const QString userDir = QStandardPaths::writableLocation(
                                QStandardPaths::GenericConfigLocation)
                            + QStringLiteral("/AetherSDR/themes");
    QDir().mkpath(userDir);
    const QString destPath = userDir + QLatin1Char('/')
                           + name + QStringLiteral(".json");

    // If a same-named user theme already exists, append a numeric suffix
    // instead of overwriting — operators sharing themes shouldn't have
    // to be paranoid about collisions destroying their existing work.
    QString finalName = name;
    QString finalPath = destPath;
    int suffix = 2;
    while (QFile::exists(finalPath)) {
        finalName = QStringLiteral("%1 (%2)").arg(name).arg(suffix++);
        finalPath = userDir + QLatin1Char('/')
                  + finalName + QStringLiteral(".json");
        if (suffix > 99) return fail(QStringLiteral("Too many collisions on name \"%1\".").arg(name));
    }

    // Rewrite "name" so the destination JSON matches its on-disk filename
    // (otherwise the editor's "Editing: …" header would show the file's
    // original name even after we suffixed it).
    QJsonObject patched = root;
    patched.insert("name", finalName);
    QFile out(finalPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Cannot write destination: %1").arg(out.errorString()));
    out.write(QJsonDocument(patched).toJson(QJsonDocument::Indented));
    out.close();

    m_themePaths.insert(finalName, finalPath);
    if (!setActiveTheme(finalName)) {
        return fail(QStringLiteral("Imported file at %1 but failed to apply it.")
                        .arg(finalPath));
    }
    return finalName;
}

QColor ThemeManager::color(const QString& token) const
{
    // Route through lookupRaw so `{primitive.key}` aliases stored in
    // semantic tokens resolve through the primitives map.  Passing ""
    // for the path reads root scope, matching legacy bare-token
    // behaviour exactly when no aliases are in play.
    const QVariant v = lookupRaw(QString(), token);
    if (!v.isValid()) {
        qCWarning(lcGui) << "ThemeManager: missing color token" << token;
        return QColor(Qt::transparent);
    }
    if (v.userType() == qMetaTypeId<ThemeGradient>()) {
        const auto g = v.value<ThemeGradient>();
        if (!g.stops.isEmpty()) return g.stops.first().color;
        return QColor(Qt::transparent);
    }
    return QColor(v.toString());
}

QBrush ThemeManager::brush(const QString& token, const QRect& bounds) const
{
    const QVariant val = lookupRaw(QString(), token);
    if (!val.isValid()) {
        qCWarning(lcGui) << "ThemeManager: missing brush token" << token;
        return QBrush(Qt::transparent);
    }
    if (val.userType() != qMetaTypeId<ThemeGradient>()) {
        // Scalar — wrap the colour into a solid brush.
        return QBrush(QColor(val.toString()));
    }
    const auto g = val.value<ThemeGradient>();
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
    // Alias-aware: lookupRaw expands `{primitive.key}` references before
    // returning, so QSS templates referencing semantic tokens that
    // alias-resolve through the primitives palette emit the right
    // literal value.
    const QVariant v = lookupRaw(QString(), token);
    if (v.isValid()) {
        if (v.userType() == qMetaTypeId<ThemeGradient>()) {
            return gradientCssFragment(v.value<ThemeGradient>());
        }
        if (v.userType() == qMetaTypeId<ThemeFont>()) {
            return v.value<ThemeFont>().family;
        }
        return colorHexToCssFragment(v.toString());
    }
    // Virtual lookup: `font.size.<role>` falls through to the embedded
    // size field on `font.family.<role>` when no direct token exists.
    // Lets QSS templates write `font-size: {{font.size.freq}}px` and have
    // edits to the freq compound's size take effect without needing a
    // separate scalar token namespace.
    if (token.startsWith(QStringLiteral("font.size."))) {
        const QString role = token.mid(QStringLiteral("font.size.").size());
        const QVariant compound = lookupRaw(QString(),
                                            QStringLiteral("font.family.") + role);
        if (compound.userType() == qMetaTypeId<ThemeFont>()) {
            const int sz = compound.value<ThemeFont>().size;
            if (sz > 0) return QString::number(sz);
        }
    }
    // Unknown token. The empty string still goes back to resolveFor(), because
    // substituting a placeholder would be worse — Qt would apply a wrong colour
    // rather than none — but it must not vanish silently.
    //
    // What the caller sees without this: `{{color.acent}}` (typo) resolves to
    // "", the template becomes `color: ;`, Qt discards the malformed
    // declaration, and the widget keeps its previous appearance. No error, no
    // log line, and the theme looks "nearly right" — which is far harder to
    // diagnose than an obviously missing colour.
    //
    // Warned once per token: resolveFor() runs on every theme change and every
    // tracked-stylesheet reapply, so an unguarded warning would flood the log.
    {
        QMutexLocker lock(&m_unknownTokenMutex);
        if (!m_warnedUnknownTokens.contains(token)) {
            m_warnedUnknownTokens.insert(token);
            qCWarning(lcGui) << "ThemeManager: stylesheet references unknown token"
                             << token << "— it resolves to an empty fragment, so the"
                             << "declaration using it will be dropped by Qt";
        }
    }
    return QString();
}

QFont ThemeManager::font(const QString& token) const
{
    // Compound-token fast path — if the operator passed a font.family.*
    // (or any token that resolved to a ThemeFont), assemble directly
    // from its family + embedded size.  Falls back to the legacy
    // composition (font.family.ui family + sizing(token)) for callers
    // that pass a font.size.* token name.
    const QVariant v = lookupRaw(QString(), token);
    if (v.userType() == qMetaTypeId<ThemeFont>()) {
        const ThemeFont tf = v.value<ThemeFont>();
        QFont qf(tf.family);
        if (tf.size > 0) qf.setPointSize(tf.size);
        return qf;
    }
    QFont f;
    const QString family = value("font.family.ui");
    if (!family.isEmpty()) f.setFamily(family);
    f.setPointSize(sizing(token));
    return f;
}

int ThemeManager::sizing(const QString& token) const
{
    const QVariant val = lookupRaw(QString(), token);
    if (val.isValid()) {
        bool ok = false;
        const int v = val.toInt(&ok);
        if (ok) return v;
        return static_cast<int>(val.toDouble());
    }
    // Virtual font.size.<role> fallback into font.family.<role>'s
    // embedded size, mirroring the cssFragment path.  Paint code that
    // composes a QFont sized off a per-role compound thus reads the
    // same number QSS templates do.
    if (token.startsWith(QStringLiteral("font.size."))) {
        const QString role = token.mid(QStringLiteral("font.size.").size());
        const QVariant compound = lookupRaw(QString(),
                                            QStringLiteral("font.family.") + role);
        if (compound.userType() == qMetaTypeId<ThemeFont>()) {
            const int sz = compound.value<ThemeFont>().size;
            if (sz > 0) return sz;
        }
    }
    qCWarning(lcGui) << "ThemeManager: missing sizing token" << token;
    return 0;
}

QString ThemeManager::value(const QString& token) const
{
    const QVariant v = lookupRaw(QString(), token);
    if (!v.isValid()) return QString();
    if (v.userType() == qMetaTypeId<ThemeGradient>()) return QString();
    // Compound font tokens transparently downgrade to their family
    // string so the ~35 sites that read `tm.value("font.family.ui")`
    // keep working after the v1-string → v2-compound migration.
    if (v.userType() == qMetaTypeId<ThemeFont>()) {
        return v.value<ThemeFont>().family;
    }
    return v.toString();
}

ThemeFont ThemeManager::fontToken(const QString& token) const
{
    return fontTokenAt(QString(), token);
}

ThemeFont ThemeManager::fontTokenAt(const QString& containerPath,
                                    const QString& token) const
{
    const QVariant v = lookupRaw(containerPath, token);
    if (v.userType() == qMetaTypeId<ThemeFont>()) return v.value<ThemeFont>();
    // Legacy v1 path: bare family string with no embedded size / color.
    ThemeFont f;
    f.family = v.toString();
    return f;
}

void ThemeManager::setFontToken(const QString& token, const ThemeFont& f)
{
    setFontToken(QString(), token, f);
}

void ThemeManager::setFontToken(const QString& containerPath,
                                const QString& token, const ThemeFont& f)
{
    ThemeScope* scope = containerPath.isEmpty()
                            ? m_rootScope.get()
                            : scopeOrCreate(containerPath);
    if (!scope) return;
    scope->tokens.insert(token, QVariant::fromValue(f));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

QString ThemeManager::resolve(const QString& stylesheetTemplate) const
{
    return resolveFor(nullptr, stylesheetTemplate);
}

QString ThemeManager::checkBoxIndicatorStyle()
{
    // Single source of truth for the checkbox indicator used across dialogs
    // (RadioSetupDialog, DxClusterDialog, FreeDvReporterDialog).  Kept as an
    // unresolved token template so applyStyleSheet() expands it per widget
    // and it re-paints on theme switches.
    static const QString kStyle =
        "QCheckBox::indicator { width: 14px; height: 14px; "
        "border: 2px solid {{color.background.3}}; border-radius: 3px; background: {{color.background.0}}; }"
        "QCheckBox::indicator:hover { border-color: {{color.accent}}; background: {{color.background.1}}; }"
        "QCheckBox::indicator:checked { border: 2px solid {{color.accent}}; background: {{color.background.2}}; }"
        "QCheckBox::indicator:disabled { border-color: {{color.background.2}}; background: {{color.background.0}}; }";
    return kStyle;
}

QString ThemeManager::resolveFor(const QWidget* widget,
                                 const QString& stylesheetTemplate) const
{
    // Replace every {{token.name}} with the token's stylesheet fragment.
    // When `widget` is non-null the token is resolved through its
    // container chain (containerPathFor → scope tree walk).  When null
    // (legacy resolve() callers) lookups go straight to root scope.
    static const QRegularExpression kRe(QStringLiteral(R"(\{\{([^}]+)\}\})"));
    QString out = stylesheetTemplate;
    QRegularExpressionMatchIterator it = kRe.globalMatch(stylesheetTemplate);
    int offset = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString token = m.captured(1).trimmed();
        const QString val = widget ? cssFragment(widget, token)
                                    : cssFragment(token);
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
    // Scope-aware resolve — the widget's container chain decides which
    // overrides apply.  Widgets with no declared ancestor fall through
    // to root scope, matching the historical flat behaviour.
    const QString resolved = resolveFor(widget, stylesheetTemplate);
    widget->setStyleSheet(resolved);

    // First-time registration: connect to destroyed() so the entry
    // disappears when the widget does, AND install ourselves as an
    // event filter so a later QEvent::ParentChange re-resolves the
    // template against the widget's now-correct scope chain.
    // (Widgets configured pre-reparent — common for helpers like
    // applyPrimarySliderStyle — would otherwise be stuck with their
    // resolved-at-no-parent root-scope QSS.)  Subsequent calls on
    // the same widget just overwrite the recorded template / token
    // list without re-installing.
    if (!m_trackedWidgets.contains(widget)) {
        connect(widget, &QObject::destroyed,
                this, &ThemeManager::onTrackedWidgetDestroyed);
        // Event filter cost: installEventFilter routes every event
        // delivered to `widget` through ThemeManager::eventFilter().
        // The handler early-outs on a single int compare for everything
        // that isn't QEvent::ParentChange, so per-event overhead is
        // negligible — but with N tracked widgets the cumulative
        // wakeups during high-frequency interaction (drag, paint
        // storms) is N×events.  If profiling ever flags this, gate the
        // install to "widgets that have no parent at apply time" since
        // those are the only ones that NEED the post-reparent fix.
        widget->installEventFilter(this);
    }
    TrackedWidget ctx;
    ctx.stylesheetTemplate = stylesheetTemplate;
    // Remember what we actually pushed so eventFilter can tell "still ours"
    // from "somebody set their own sheet afterwards".
    ctx.appliedStylesheet  = resolved;
    ctx.tokens = extractReferencedTokens(stylesheetTemplate);
    m_trackedWidgets.insert(widget, ctx);
}

bool ThemeManager::eventFilter(QObject* watched, QEvent* event)
{
    // Polish is delivered to a widget when Qt first prepares it for display —
    // by which time it IS in its final parent chain, however it got there.
    //
    // ParentChange alone is not enough, and #4520 is why: the filter is only
    // installed on TRACKED widgets, so when an untracked ANCESTOR is reparented
    // the tracked child inside it hears nothing. Real construction code
    // produces exactly that shape:
    //
    //     stack = new QStackedWidget;    // no parent
    //     label = new QLabel;            // no parent
    //     applyStyleSheet(label, ...);   // resolves at ROOT — wrong scope
    //     stack->addWidget(label);       // ParentChange on the LABEL, but the
    //                                    // stack is still an orphan
    //     layout->addWidget(stack);      // ParentChange on the STACK, which is
    //                                    // untracked → the label never re-resolves
    //
    // The label then keeps root-scope values until the next themeChanged(). On
    // the VFO flag that meant a band change (which destroys and rebuilds the
    // flag) silently reverted the operator's chosen frequency font, while
    // touching anything in the Theme Editor appeared to "fix" it.
    //
    // Show / ShowToParent are the ones that actually catch it: they arrive when
    // the widget first becomes visible, by which point it is unavoidably in its
    // final chain no matter how it got there. Polish alone does NOT rescue this
    // shape — it fires at most once per widget, and in the stack case above it
    // is spent before the stack joins its scoped host (drop Show/ShowToParent
    // and the #4520 regression test goes red). Polish still earns its place: it
    // is what reaches a QStackedWidget page that is never the *current* one, so
    // VfoWidget's edit-mode m_freqEdit gets the scoped font too, not just the
    // visible m_freqLabel. ParentChange stays as the earliest and cheapest
    // signal for the direct case.
    //
    // Cost: ParentChange and Polish are once-ish per widget, but Show and
    // ShowToParent BOTH fire on every show transition — a VFO flag
    // collapse/expand (VfoWidget::setCollapsed) bulk-toggles visibility across
    // the whole subtree, so this is not a rare event. The re-resolve is kept
    // cheap by the no-op guard below: resolveFor() is a regex pass over a short
    // template, and setStyleSheet() (which drives a full QStyleSheetStyle
    // repolish) only runs when the resolved QSS actually changed.
    if (event->type() == QEvent::ParentChange
        || event->type() == QEvent::Polish
        || event->type() == QEvent::Show
        || event->type() == QEvent::ShowToParent) {
        QWidget* w = qobject_cast<QWidget*>(watched);
        if (w) {
            const auto it = m_trackedWidgets.constFind(w);
            if (it != m_trackedWidgets.constEnd()
                && !it.value().stylesheetTemplate.isEmpty()
                // Registration through applyStyleSheet() does NOT mean the
                // widget is still wearing what we gave it.  The house pattern
                // "register a generic themed look, then overwrite it directly
                // with per-state colour" is everywhere — RxApplet's per-slice
                // badge colour, VfoWidget's split badge and RADE SNR label, the
                // status bar's TX indicator (which would otherwise repaint
                // itself TX-red while the radio is receiving).  Re-resolving
                // over one of those wipes real state, so only re-resolve while
                // the widget still carries exactly the QSS we last applied.
                && w->styleSheet() == it.value().appliedStylesheet) {
                const QString resolved =
                    resolveFor(w, it.value().stylesheetTemplate);
                // No-op guard: Show + ShowToParent fire as a pair on every
                // show, so without this each toggle costs two full repolishes
                // per tracked widget for an identical result.
                if (resolved != it.value().appliedStylesheet) {
                    // Record BEFORE setStyleSheet: the repolish it triggers can
                    // re-enter arbitrary widget code that touches
                    // m_trackedWidgets, which would invalidate `it`.
                    m_trackedWidgets[w].appliedStylesheet = resolved;
                    w->setStyleSheet(resolved);
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void ThemeManager::clearWidgetTracking(QWidget* widget)
{
    if (!widget) return;
    if (m_trackedWidgets.remove(widget)) {
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
    // Smart invalidation: when this re-apply is the consequence of a
    // single-token edit (setColor / setGradient / setSizing / setString),
    // m_currentEditToken is set and we skip every widget whose recorded
    // template doesn't reference that token.  For 500+ tracked widgets
    // and a typical token consumed by ~5–10 sites, this is a 50–100x
    // speedup over the full walk and is the difference between a
    // smooth picker drag and "every mouse move stalls the UI".
    // setActiveTheme leaves m_currentEditToken empty so the full theme
    // switch still touches every tracked stylesheet.
    const bool targeted = !m_currentEditToken.isEmpty();
    for (QWidget* w : widgets) {
        const auto it = m_trackedWidgets.constFind(w);
        if (it == m_trackedWidgets.constEnd()) continue;  // dropped mid-sweep
        // Paint-code widgets registered via declareWidgetTokens() have an
        // empty template — skip them so we don't wipe any stylesheet they
        // may have inherited from a parent / Theme.h helper.  They handle
        // theme changes by connecting to themeChanged themselves.
        const auto& ctx = it.value();
        if (ctx.stylesheetTemplate.isEmpty()) continue;
        if (targeted && !ctx.tokens.contains(m_currentEditToken)) continue;
        // Scope-aware re-apply — same path as applyStyleSheet() so an
        // edit at a non-root scope visibly takes effect for every
        // tracked widget under that container.
        //
        // Unlike eventFilter this re-applies unconditionally, including over
        // a sheet a caller set directly — a theme switch is an explicit,
        // user-initiated repaint and that has always been its behaviour.
        // Recording it keeps eventFilter's "still ours" test meaningful; skip
        // this and the guard would latch off permanently after the first
        // theme change.  Record before setStyleSheet(), which can re-enter
        // widget code that mutates m_trackedWidgets.
        const QString resolved = resolveFor(w, ctx.stylesheetTemplate);
        m_trackedWidgets[w].appliedStylesheet = resolved;
        w->setStyleSheet(resolved);
    }
}

// ─────────────────────────────────────── scope-aware public API ─────────

QString ThemeManager::containerPathFor(const QWidget* widget) const
{
    // Walk the Qt parent chain looking for the nearest declared
    // themeContainer property.  An empty / missing property is treated
    // as "no declaration here" — the walk continues past it.  Returns
    // an empty string (== root scope) if no ancestor declares one.
    const QWidget* w = widget;
    while (w) {
        const QVariant prop = w->property("themeContainer");
        if (prop.isValid()) {
            const QString s = prop.toString();
            if (!s.isEmpty()) return s;
        }
        w = w->parentWidget();
    }
    return QString();
}

void ThemeManager::registerDeclaredContainer(const QString& containerPath)
{
    if (containerPath.isEmpty()) return;
    m_declaredContainers.insert(containerPath);
    // Ensure the scope exists in the tree even when no override is
    // present yet — keeps the path visible to the editor's tree
    // picker between theme loads.
    scopeOrCreate(containerPath);
}

QColor ThemeManager::colorAt(const QString& containerPath, const QString& token) const
{
    const QVariant v = lookupRaw(containerPath, token);
    if (!v.isValid()) return color(token);
    if (v.userType() == qMetaTypeId<ThemeGradient>()) {
        const ThemeGradient g = v.value<ThemeGradient>();
        return g.stops.isEmpty() ? QColor() : g.stops.first().color;
    }
    return QColor(v.toString());
}

int ThemeManager::sizingAt(const QString& containerPath, const QString& token) const
{
    const QVariant v = lookupRaw(containerPath, token);
    if (!v.isValid()) return sizing(token);
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : sizing(token);
}

QString ThemeManager::valueAt(const QString& containerPath, const QString& token) const
{
    const QVariant v = lookupRaw(containerPath, token);
    if (!v.isValid()) return value(token);
    if (v.userType() == qMetaTypeId<ThemeGradient>()) return {};
    // Compound font tokens transparently downgrade to .family — same
    // contract as the bare `value(token)` overload.
    if (v.userType() == qMetaTypeId<ThemeFont>()) {
        return v.value<ThemeFont>().family;
    }
    return v.toString();
}

ThemeGradient ThemeManager::gradientAt(const QString& containerPath, const QString& token) const
{
    const QVariant v = lookupRaw(containerPath, token);
    if (v.userType() == qMetaTypeId<ThemeGradient>()) return v.value<ThemeGradient>();
    return gradient(token);
}

bool ThemeManager::isOverriddenAt(const QString& containerPath, const QString& token) const
{
    const ThemeScope* s = scopeForPath(containerPath);
    if (!s) return false;
    return s->tokens.constFind(token) != s->tokens.constEnd();
}

void ThemeManager::removeOverride(const QString& containerPath, const QString& token)
{
    ThemeScope* s = scopeForPath(containerPath);
    if (!s) return;
    // Root scope is the BASE — there's nothing for the token to
    // inherit FROM if we drop its root entry, so a "clear" there would
    // delete the value tree-wide rather than restore inheritance.
    // Reject root-scope clears defensively; the editor surface
    // already hides the menu for the root column.
    if (s == m_rootScope.get()) {
        qCWarning(lcGui) << "ThemeManager::removeOverride: refusing to drop"
                         << token << "from root scope (would delete it tree-wide)";
        return;
    }
    if (s->tokens.remove(token) == 0) return;  // nothing to drop
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

QStringList ThemeManager::containerPaths() const
{
    QStringList out;
    out.reserve(m_scopeByPath.size());
    std::function<void(const ThemeScope*)> walk = [&](const ThemeScope* s) {
        if (s == m_rootScope.get()) {
            out.append(QString());
        } else {
            out.append(s->path);
        }
        // Deterministic alphabetical order at each level.
        // std::map iterates in key order already — pass children
        // through directly.
        for (auto& kv : s->children) walk(kv.second.get());
    };
    walk(m_rootScope.get());
    return out;
}

QColor ThemeManager::color(const QWidget* widget, const QString& token) const
{
    const QVariant v = lookupRaw(containerPathFor(widget), token);
    if (!v.isValid()) {
        // Same fallback rules as the bare-token color() overload.
        return color(token);
    }
    if (v.userType() == qMetaTypeId<ThemeGradient>()) {
        const ThemeGradient g = v.value<ThemeGradient>();
        return g.stops.isEmpty() ? QColor() : g.stops.first().color;
    }
    return QColor(v.toString());
}

int ThemeManager::sizing(const QWidget* widget, const QString& token) const
{
    const QVariant v = lookupRaw(containerPathFor(widget), token);
    if (!v.isValid()) return sizing(token);
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : sizing(token);
}

QFont ThemeManager::font(const QWidget* widget, const QString& token) const
{
    Q_UNUSED(widget);
    // Font tokens currently resolve as plain string family + a fixed
    // size in the bare-token overload.  Scope-aware font resolution
    // (where a container can override "font.family.ui" or
    // "font.size.normal") falls through to the legacy code path
    // unchanged until PR 4 starts populating container scopes.  The
    // widget-aware lookup wired through containerPathFor will still
    // pick up any overrides for tokens defined under nested scopes
    // when callers route through here.
    return font(token);
}

QString ThemeManager::value(const QWidget* widget, const QString& token) const
{
    const QVariant v = lookupRaw(containerPathFor(widget), token);
    if (!v.isValid()) return value(token);
    if (v.userType() == qMetaTypeId<ThemeGradient>()) return {};
    return v.toString();
}

QBrush ThemeManager::brush(const QWidget* widget, const QString& token,
                           const QRect& bounds) const
{
    Q_UNUSED(widget);
    // brush() / cssFragment() build off the same token resolution that
    // bare-token color()/gradient() use — for now route through the
    // legacy overload.  PR 4 wires this up to scope-aware lookup once
    // gradient tokens start living under non-root scopes.
    return brush(token, bounds);
}

QString ThemeManager::cssFragment(const QWidget* widget, const QString& token) const
{
    // Scope-walk via the widget's container chain.  Falls back to root
    // when no scope sets the token — keeps every QSS template valid
    // even when the active theme has no per-container overrides yet.
    const QVariant v = lookupRaw(containerPathFor(widget), token);
    if (v.isValid()) {
        if (v.userType() == qMetaTypeId<ThemeGradient>()) {
            return gradientCssFragment(v.value<ThemeGradient>());
        }
        if (v.userType() == qMetaTypeId<ThemeFont>()) {
            return v.value<ThemeFont>().family;
        }
        return colorHexToCssFragment(v.toString());
    }
    // Virtual font.size.<role> → font.family.<role>'s embedded size,
    // walking the widget's scope chain so the freq label can vary per
    // applet too.
    if (token.startsWith(QStringLiteral("font.size."))) {
        const QString role = token.mid(QStringLiteral("font.size.").size());
        const QVariant compound = lookupRaw(containerPathFor(widget),
                                            QStringLiteral("font.family.") + role);
        if (compound.userType() == qMetaTypeId<ThemeFont>()) {
            const int sz = compound.value<ThemeFont>().size;
            if (sz > 0) return QString::number(sz);
        }
    }
    return cssFragment(token);
}

ThemeGradient ThemeManager::gradient(const QWidget* widget, const QString& token) const
{
    const QVariant v = lookupRaw(containerPathFor(widget), token);
    if (v.userType() == qMetaTypeId<ThemeGradient>()) return v.value<ThemeGradient>();
    return gradient(token);
}

void ThemeManager::setColor(const QString& containerPath, const QString& token, const QColor& color)
{
    if (!color.isValid()) return;
    if (containerPath.isEmpty()) {
        setColor(token, color);  // route through the root-scope overload
        return;
    }
    const QString hex = colorToTokenString(color);
    ThemeScope* scope = scopeOrCreate(containerPath);
    const auto it = scope->tokens.constFind(token);
    if (it != scope->tokens.constEnd() && it.value().toString() == hex) return;
    scope->tokens.insert(token, QVariant(hex));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

void ThemeManager::setSizing(const QString& containerPath, const QString& token, int v)
{
    if (containerPath.isEmpty()) { setSizing(token, v); return; }
    ThemeScope* scope = scopeOrCreate(containerPath);
    const auto it = scope->tokens.constFind(token);
    if (it != scope->tokens.constEnd() && it.value().toInt() == v) return;
    scope->tokens.insert(token, QVariant(v));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

void ThemeManager::setGradient(const QString& containerPath, const QString& token, const ThemeGradient& g)
{
    if (containerPath.isEmpty()) { setGradient(token, g); return; }
    ThemeScope* scope = scopeOrCreate(containerPath);
    scope->tokens.insert(token, QVariant::fromValue(g));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

void ThemeManager::setString(const QString& containerPath, const QString& token, const QString& v)
{
    if (containerPath.isEmpty()) { setString(token, v); return; }
    ThemeScope* scope = scopeOrCreate(containerPath);
    const auto it = scope->tokens.constFind(token);
    if (it != scope->tokens.constEnd()
        && it.value().userType() == QMetaType::QString
        && it.value().toString() == v) {
        return;
    }
    scope->tokens.insert(token, QVariant(v));
    m_currentEditToken = token;
    emit themeChanged();
    m_currentEditToken.clear();
    saveActiveTheme();
}

namespace theme {
void setContainer(QWidget* widget, const QString& containerPath)
{
    if (!widget) return;
    widget->setProperty("themeContainer", containerPath);
    // Register the declared path with ThemeManager so it stays visible
    // in the editor's container tree even when no override has been
    // written to that scope yet.
    if (!containerPath.isEmpty()) {
        ThemeManager::instance().registerDeclaredContainer(containerPath);
    }
}

QString containerOf(const QWidget* widget)
{
    if (!widget) return {};
    return widget->property("themeContainer").toString();
}
} // namespace theme

} // namespace AetherSDR
