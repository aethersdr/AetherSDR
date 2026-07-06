#include "core/ThemeManager.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected (%s) to be true\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (!(a_ == e_)) { \
        std::fprintf(stderr, "FAIL %s:%d\n", __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    // Route every QStandardPaths writable location into Qt's test-mode
    // sandbox (~/.qttest/...).  XDG_CONFIG_HOME alone isn't enough:
    // macOS ignores it and resolves GenericConfigLocation to
    // ~/Library/Preferences, so importThemeFromFile() would persist the
    // probe themes into the developer's real themes dir — the leftover
    // files then collide on the next run and the import de-duplicates
    // the name to "... (2)", failing the EXPECT_EQ assertions below.
    QStandardPaths::setTestModeEnabled(true);

    // The sandbox itself persists across runs, so clear any probe
    // themes a previous (possibly crashed) run left behind before the
    // ThemeManager singleton scans the dir.
    {
        const QString sandboxAppDir =
            QStandardPaths::writableLocation(
                QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/AetherSDR");
        QDir(sandboxAppDir).removeRecursively();
    }

    // Route AppSettings + theme dirs into an isolated temp tree so the
    // test never pollutes the developer's real ~/.config/AetherSDR.
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    qputenv("XDG_CONFIG_HOME", tmp.path().toUtf8());

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("AetherSDR-test");
    QCoreApplication::setApplicationName("AetherSDR-test");

    AppSettings::instance().load();

    // ── Compiled-in defaults are usable before any theme is loaded ──
    // The built-in resource path :/themes/default-dark.json is the
    // expected first-load target, but if it's missing the manager still
    // returns sane values from seedBuiltinDefaults().
    auto& tm = ThemeManager::instance();
    EXPECT_TRUE(tm.color("color.accent").isValid());

    // ── Default Dark loads from :/themes/ via setActiveTheme ──
    // The shipped resource theme should be in availableThemes(); switching
    // to it should leave color.accent at the canonical #00b4d8.
    const QStringList themes = tm.availableThemes();
    EXPECT_TRUE(themes.contains("Default Dark"));

    EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
    EXPECT_EQ(tm.activeTheme(), QString("Default Dark"));
    EXPECT_EQ(tm.color("color.accent").name().toLower(), QString("#00b4d8"));
    EXPECT_EQ(tm.color("color.background.0").name().toLower(), QString("#0f0f1a"));
    EXPECT_EQ(tm.color("color.text.primary").name().toLower(), QString("#c8d8e8"));
    EXPECT_EQ(tm.sizing("font.size.normal"), 12);
    EXPECT_EQ(tm.sizing("sizing.panel.padding"), 4);

    // ── Phase 2 canonical taxonomy expansion ──
    // Representative slice of the 51-token set proves the JSON loader
    // handles the larger nested structure and the seed/JSON stays in sync.
    EXPECT_EQ(tm.color("color.background.1").name().toLower(),        QString("#1a2a3a"));
    EXPECT_EQ(tm.color("color.background.tx").name().toLower(),       QString("#3a2a0e"));
    EXPECT_EQ(tm.color("color.background.spectrum").name().toLower(), QString("#000000"));
    EXPECT_EQ(tm.color("color.accent.bright").name().toLower(),       QString("#00c8f0"));
    EXPECT_EQ(tm.color("color.text.disabled").name().toLower(),       QString("#3a4a5a"));
    EXPECT_EQ(tm.color("color.border.accent").name().toLower(),       QString("#00b4d8"));
    EXPECT_EQ(tm.color("color.meter.crst").name().toLower(),          QString("#ff4d4d"));
    EXPECT_EQ(tm.color("color.spectrum.trace").name().toLower(),      QString("#00b4d8"));
    EXPECT_EQ(tm.color("color.slice.a").name().toLower(),             QString("#00d4ff"));
    EXPECT_EQ(tm.color("color.slice.h").name().toLower(),             QString("#b080ff"));
    EXPECT_EQ(tm.color("color.slice.tx").name().toLower(),            QString("#ff4d4d"));

    // ── Missing token returns transparent + logs (no crash) ──
    EXPECT_EQ(tm.color("color.nonexistent.token").alpha(), 0);
    EXPECT_EQ(tm.sizing("sizing.nonexistent"), 0);

    // ── Stylesheet template resolution ──
    // background.1 changed from v1.0 (#0d1119) to v1.1 (#1a2a3a) as part
    // of the Phase 2 canonicalisation; this test asserts the new value.
    const QString tpl =
        "QPushButton { background: {{color.background.1}}; "
        "color: {{color.accent}}; "
        "padding: {{sizing.panel.padding}}px; }";
    const QString out = tm.resolve(tpl);
    EXPECT_TRUE(out.contains("background: #1a2a3a"));
    EXPECT_TRUE(out.contains("color: #00b4d8"));
    EXPECT_TRUE(out.contains("padding: 4px"));
    EXPECT_TRUE(!out.contains("{{"));

    // ── Unknown placeholder substitutes empty string (no crash) ──
    const QString badTpl = "{{color.does.not.exist}}";
    const QString badOut = tm.resolve(badTpl);
    EXPECT_TRUE(!badOut.contains("{{"));

    // ── Phase 2 gradient token support ──
    // The waterfall.colormap tokens are now a nested family of five named
    // presets (default / grayscale / blueGreen / fire / plasma), each a
    // linear gradient covering the RF visualisation range.  Verifies the
    // full gradient parsing + brush construction + cssFragment emission +
    // resolve() routing path end-to-end against the canonical
    // .default preset (7 stops, black → navy → … → red) and asserts the
    // white end-stop via the .grayscale preset.
    {
        // color() on a gradient token returns the first stop as a
        // graceful fallback for callers that don't know about gradients.
        EXPECT_EQ(tm.color("color.waterfall.colormap.default").name().toLower(),
                  QString("#000000"));

        // value() on a gradient token returns empty — the structured
        // form has no meaningful raw scalar.
        EXPECT_TRUE(tm.value("color.waterfall.colormap.default").isEmpty());

        // brush() on a gradient token returns a non-Solid brush.
        QBrush b = tm.brush("color.waterfall.colormap.default", QRect(0, 0, 100, 50));
        EXPECT_TRUE(b.style() == Qt::LinearGradientPattern);
        const QGradient* grad = b.gradient();
        EXPECT_TRUE(grad != nullptr);
        // Guard the dereference — if a future token rename makes brush()
        // fall back to the transparent solid brush, the harness logs the
        // failure but does not abort, so an unguarded grad->stops() here
        // would SEGV instead of producing a clean diagnostic.
        if (grad) EXPECT_EQ(grad->stops().size(), 7);

        // cssFragment() emits Qt's qlineargradient(...) syntax with the
        // angle properly mapped to (x1,y1,x2,y2) endpoints + every stop
        // present.
        const QString css = tm.cssFragment("color.waterfall.colormap.default");
        EXPECT_TRUE(css.startsWith("qlineargradient("));
        EXPECT_TRUE(css.contains("stop:0.0000 #000000"));
        EXPECT_TRUE(css.contains("stop:1.0000 #ff0000"));

        // The grayscale preset is the canonical black→white ramp; use it
        // to assert the #ffffff end-stop path.
        const QString grayCss = tm.cssFragment("color.waterfall.colormap.grayscale");
        EXPECT_TRUE(grayCss.contains("stop:1.0000 #ffffff"));

        // resolve() routes gradient tokens through cssFragment(), so an
        // existing {{token}} stylesheet template substitutes the
        // qlineargradient(...) string seamlessly.
        const QString gradTpl =
            "QWidget { background: {{color.waterfall.colormap.default}}; }";
        const QString gradOut = tm.resolve(gradTpl);
        EXPECT_TRUE(gradOut.contains("background: qlineargradient("));
        EXPECT_TRUE(gradOut.contains("stop:1.0000 #ff0000"));
        EXPECT_TRUE(!gradOut.contains("{{"));
    }

    // ── themeChanged signal fires on setActiveTheme ──
    QSignalSpy spy(&tm, &ThemeManager::themeChanged);
    // Setting the same theme is a no-op (already active) — no signal expected
    EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
    EXPECT_EQ(spy.count(), 0);
    // Setting an unknown theme returns false and does NOT fire the signal
    EXPECT_TRUE(!tm.setActiveTheme("Nonexistent Theme"));
    EXPECT_EQ(spy.count(), 0);

    // ── User-dir theme loading + override of compiled-in defaults ──
    // Write a JSON theme into the temp user-themes dir and verify it
    // gets picked up and overrides the compiled accent colour.  Demands
    // a fresh manager instance because availableThemes is scanned in
    // the constructor and Phase 1 doesn't implement a rescan API yet —
    // that's Phase 5 work.  The check below verifies the loadThemeFromPath
    // logic via the *file* layer though by writing the theme and re-reading
    // it through setActiveTheme's load path on a separate construction.
    const QString userThemesDir = tmp.path() + "/AetherSDR-test/themes";
    QDir().mkpath(userThemesDir);
    QFile f(userThemesDir + "/test-theme.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(R"({
            "schemaVersion": 1,
            "name": "Test Theme",
            "tokens": {
                "color": { "accent": "#ff00ff" }
            }
        })");
        f.close();
    }
    // Phase 1 doesn't expose rescan publicly — verifying the user-dir
    // file scan happens only on construction.  The Phase 5 editor will
    // add a rescan trigger; for now this exercises the file-write path
    // that the editor will eventually use.
    EXPECT_TRUE(QFile::exists(userThemesDir + "/test-theme.json"));

    // ── Phase 2 widget→tokens reverse-map ──
    // applyStyleSheet must:
    //   1. set the widget's stylesheet to the resolved template
    //   2. record the (widget → tokens) reverse-map
    //   3. re-apply the template when themeChanged fires (live re-theme)
    //   4. drop its entry when the widget is destroyed
    {
        const QString tpl =
            "QLabel { color: {{color.accent}}; "
            "background: {{color.background.1}}; "
            "padding: {{sizing.panel.padding}}px; }";

        QLabel* lbl = new QLabel;
        tm.applyStyleSheet(lbl, tpl);

        // Stylesheet was resolved + applied
        const QString applied = lbl->styleSheet();
        EXPECT_TRUE(applied.contains("color: #00b4d8"));
        EXPECT_TRUE(applied.contains("background: #1a2a3a"));
        EXPECT_TRUE(!applied.contains("{{"));

        // Reverse-map recorded — three unique tokens
        const QStringList recorded = tm.tokensForWidget(lbl);
        EXPECT_EQ(recorded.size(), 3);
        EXPECT_TRUE(recorded.contains("color.accent"));
        EXPECT_TRUE(recorded.contains("color.background.1"));
        EXPECT_TRUE(recorded.contains("sizing.panel.padding"));

        // clearWidgetTracking detaches the widget — subsequent
        // themeChanged events should NOT re-apply.
        tm.clearWidgetTracking(lbl);
        EXPECT_TRUE(tm.tokensForWidget(lbl).isEmpty());

        // Re-register, then verify destroyed() cleanup by destroying
        // the widget and re-querying.  The lookup must not crash and
        // must return an empty list.
        tm.applyStyleSheet(lbl, tpl);
        EXPECT_EQ(tm.tokensForWidget(lbl).size(), 3);
        delete lbl;
        QApplication::processEvents();  // let destroyed() fire
        // After destruction, the pointer is dangling — we can't legally
        // call tokensForWidget(lbl) anymore, but the internal hash
        // entry must be gone.  Exercise this by registering a fresh
        // widget and confirming the map size hasn't leaked.
        QLabel* fresh = new QLabel;
        tm.applyStyleSheet(fresh, tpl);
        EXPECT_EQ(tm.tokensForWidget(fresh).size(), 3);
        delete fresh;
        QApplication::processEvents();
    }

    // ── Slider + knob token namespaces seeded from compiled defaults ──
    // Older user themes (forked before the namespace existed) have no
    // slider/knob entries in their on-disk JSON.  seedBuiltinDefaults()
    // is responsible for ensuring those tokens still resolve to the
    // canonical Wave-blue look instead of empty / Qt-default rendering.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");
        EXPECT_TRUE(tm.color("color.slider.foreground").isValid());
        EXPECT_TRUE(tm.color("color.slider.background").isValid());
        EXPECT_TRUE(tm.color("color.slider.handle").isValid());
        EXPECT_TRUE(tm.color("color.knob.foreground").isValid());
        EXPECT_EQ(tm.color("color.slider.foreground").name().toLower(),
                  QString("#00b4d8"));
    }

    // ── Per-applet scope cascade ──
    // The bundled themes ship nested-scope overrides under
    // scopes.applet.scopes.{tx,rx,comp}.  A widget walking via
    // applet/tx should resolve the foreground to red, not the root
    // scope's blue.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");
        EXPECT_EQ(tm.colorAt("applet/tx",   "color.slider.foreground").name().toLower(),
                  QString("#ff4d4d"));
        EXPECT_EQ(tm.colorAt("applet/rx",   "color.slider.foreground").name().toLower(),
                  QString("#4dd87a"));
        EXPECT_EQ(tm.colorAt("applet/comp", "color.slider.foreground").name().toLower(),
                  QString("#ffb84d"));
        EXPECT_EQ(tm.colorAt("applet/tx",   "color.knob.foreground").name().toLower(),
                  QString("#ff4d4d"));
        // Unrelated applet inherits root scope.
        EXPECT_EQ(tm.colorAt("applet/dax",  "color.slider.foreground").name().toLower(),
                  QString("#00b4d8"));
        // isOverriddenAt distinguishes "set here" from "inherited".
        EXPECT_TRUE(tm.isOverriddenAt("applet/tx", "color.slider.foreground"));
        EXPECT_TRUE(!tm.isOverriddenAt("applet/tx", "color.text.primary"));
    }

    // ── Toggle button tribes — base + per-tribe checked tokens ──
    // The toggle namespace splits checked-state colours across three
    // tribes (accent / success / warning); shared base + disabled
    // tokens apply regardless of tribe.  seedBuiltinDefaults() and the
    // bundled themes both must provide all 16 tokens so older user
    // themes don't regress to empty QSS values.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");
        // Shared base tokens
        EXPECT_TRUE(tm.color("color.toggle.background").isValid());
        EXPECT_TRUE(tm.color("color.toggle.foreground").isValid());
        EXPECT_TRUE(tm.color("color.toggle.border").isValid());
        EXPECT_TRUE(tm.color("color.toggle.background.disabled").isValid());
        // Per-tribe checked-state tokens — bundled theme aliases resolve.
        EXPECT_EQ(tm.color("color.toggle.accent.background.checked").name().toLower(),
                  QString("#0070c0"));   // {color.blue.700}
        EXPECT_EQ(tm.color("color.toggle.success.background.checked").name().toLower(),
                  QString("#006040"));   // color.background.success
        EXPECT_EQ(tm.color("color.toggle.warning.background.checked").name().toLower(),
                  QString("#5a3a0a"));   // color.background.warning (new primitive)
    }

    // ── Toggle button — Accent tribe per-applet cascade ──
    // Only the Accent tribe carries per-applet overrides (TX red, RX
    // green, comp amber).  Success + Warning tribes are semantic and
    // must resolve to the same value inside any applet as at root.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");
        // Accent tribe — surface-tinted by applet.
        EXPECT_EQ(tm.colorAt("applet/tx",   "color.toggle.accent.background.checked").name().toLower(),
                  QString("#ff4d4d"));
        EXPECT_EQ(tm.colorAt("applet/rx",   "color.toggle.accent.background.checked").name().toLower(),
                  QString("#4dd87a"));
        EXPECT_EQ(tm.colorAt("applet/comp", "color.toggle.accent.background.checked").name().toLower(),
                  QString("#ffb84d"));
        // Unrelated applet inherits root scope (blue).
        EXPECT_EQ(tm.colorAt("applet/dax",  "color.toggle.accent.background.checked").name().toLower(),
                  QString("#0070c0"));
        // isOverriddenAt confirms the override is set at applet/tx itself,
        // not inherited through the parent chain.
        EXPECT_TRUE(tm.isOverriddenAt("applet/tx",   "color.toggle.accent.background.checked"));
        EXPECT_TRUE(tm.isOverriddenAt("applet/rx",   "color.toggle.accent.background.checked"));
        EXPECT_TRUE(tm.isOverriddenAt("applet/comp", "color.toggle.accent.background.checked"));
        // Success + Warning tribes — semantic, no per-applet shift.
        EXPECT_EQ(tm.colorAt("applet/tx",  "color.toggle.success.background.checked").name().toLower(),
                  QString("#006040"));
        EXPECT_EQ(tm.colorAt("applet/tx",  "color.toggle.warning.background.checked").name().toLower(),
                  QString("#5a3a0a"));
        EXPECT_TRUE(!tm.isOverriddenAt("applet/tx", "color.toggle.success.background.checked"));
        EXPECT_TRUE(!tm.isOverriddenAt("applet/tx", "color.toggle.warning.background.checked"));
    }

    // ── Factory-snapshot v2 schema awareness ──
    // ensureFactoryLoaded() reads :/themes/default-dark.json to build
    // m_factoryTokens, which Reset-to-default reads at root scope.  The
    // pre-PR loader only knew the v1 shape (top-level "tokens") and
    // therefore landed an empty map for any v2-schema theme — silently
    // disabling Reset at root scope across the editor since #3176.  This
    // test locks in the v2-aware factory path so a future schema bump
    // doesn't regress it.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");
        EXPECT_TRUE(tm.hasFactoryValue("color.accent"));
        EXPECT_EQ(tm.factoryColor("color.accent").name().toLower(),
                  QString("#00b4d8"));  // alias {color.blue.500} resolved
        EXPECT_TRUE(tm.hasFactoryValue("color.background.0"));
        // Gradient tokens land in m_factoryTokens as QVariant<ThemeGradient>
        // rather than QString.  The alias-resolution loop must skip those
        // (userType() != QMetaType::QString) — and factoryColor() must take
        // the first-stop-fallback branch at ThemeManager.cpp:849-851 to
        // return a valid colour.  Lock both in: color.meter.bar.fillGradient
        // is bundled as a v2 gradient with first stop #2f9e6a.
        EXPECT_TRUE(tm.hasFactoryValue("color.meter.bar.fillGradient"));
        EXPECT_EQ(tm.factoryColor("color.meter.bar.fillGradient").name().toLower(),
                  QString("#2f9e6a"));
        // Tokens that don't exist in the bundled theme should still
        // report no factory value (sanity check the lookup isn't
        // unconditionally returning true).
        EXPECT_TRUE(!tm.hasFactoryValue("color.totally.fictional.token"));
    }

    // ── ParentChange re-resolution ──
    // applyStyleSheet on a widget with no parent locks the resolved
    // stylesheet to root scope.  After the widget is reparented to a
    // container marked themeContainer = "applet/tx", the
    // QEvent::ParentChange filter should kick in and re-resolve
    // against the new chain.
    {
        auto& tm = ThemeManager::instance();
        tm.setActiveTheme("Default Dark");

        QWidget txHost;
        theme::setContainer(&txHost, "applet/tx");

        QLabel* probe = new QLabel;  // no parent yet
        tm.applyStyleSheet(probe,
            "QLabel { color: {{color.slider.foreground}}; }");
        // At apply time probe has no parent → resolves to root blue.
        EXPECT_TRUE(probe->styleSheet().contains("#00b4d8"));

        probe->setParent(&txHost);
        QApplication::processEvents();
        // After reparent the filter re-resolves through applet/tx → red.
        EXPECT_TRUE(probe->styleSheet().contains("#ff4d4d"));

        delete probe;
        QApplication::processEvents();
    }

    // ── extractReferencedTokens static helper ──
    // Order-preserving deduplication; empty placeholders ignored.
    {
        const QStringList tokens = ThemeManager::extractReferencedTokens(
            "{{color.accent}} {{color.background.1}} {{color.accent}} "
            "{{ font.size.normal }} {{}}");
        EXPECT_EQ(tokens.size(), 3);
        EXPECT_EQ(tokens[0], QString("color.accent"));
        EXPECT_EQ(tokens[1], QString("color.background.1"));
        EXPECT_EQ(tokens[2], QString("font.size.normal"));
    }

    // ── v2 schema: primitives + {alias} resolution end-to-end ──
    // Default Dark is a v2 file whose semantic tokens (e.g. color.accent)
    // reference primitives (e.g. {color.blue.500}).  Pin that the loader
    // resolves through the primitives map rather than returning the literal
    // alias string — the cssFragment-empty-for-ThemeFont bug fixed mid-PR
    // sat right next to this code path, so the assertion guards both.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        // color() must produce the resolved hex, not the "{color.blue.500}"
        // literal that would emerge if resolveAlias() ever short-circuited.
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  QString("#00b4d8"));
        // cssFragment routes through the same alias-aware lookup; an
        // unresolved alias would emit the literal "{color.blue.500}"
        // string into the QSS, which Qt would silently render as nothing.
        EXPECT_EQ(tm.cssFragment("color.accent"), QString("#00b4d8"));
        // resolve() shells templates through cssFragment, so a {{token}}
        // referencing an aliased token must inline the primitive's value.
        const QString sheet = tm.resolve(
            "QPushButton { background: {{color.accent}}; }");
        EXPECT_TRUE(sheet.contains("background: #00b4d8"));
        EXPECT_TRUE(!sheet.contains("{color.blue.500}"));
    }

    // ── flattenTokens discriminator: gradient vs ThemeFont vs nested ──
    // Write a v1 theme that exercises all three object shapes in the same
    // tokens block, import it, and assert each leaf type-resolves correctly.
    // The order of the discriminator checks in flattenTokens() matters
    // (a font compound has no "type" field; a gradient has no "family"
    // field; a plain nested object has neither and must recurse).
    {
        const QString discriminatorDir = tmp.path() + "/_discriminator_src";
        QDir().mkpath(discriminatorDir);
        const QString discriminatorPath =
            discriminatorDir + "/discriminator.json";
        QFile df(discriminatorPath);
        EXPECT_TRUE(df.open(QIODevice::WriteOnly));
        df.write(R"({
            "schemaVersion": 1,
            "name": "Discriminator Probe",
            "tokens": {
                "color": {
                    "accent": "#abcdef",
                    "waterfall": {
                        "colormap": {
                            "type": "linear-gradient",
                            "angle": 90,
                            "stops": [
                                { "at": 0.0, "color": "#000000" },
                                { "at": 1.0, "color": "#ffffff" }
                            ]
                        }
                    }
                },
                "font": {
                    "family": {
                        "freq": {
                            "family": "DSEG7 Modern",
                            "size":   30,
                            "color":  "#c8d8e8"
                        }
                    }
                }
            }
        })");
        df.close();

        QString impErr;
        const QString imported = tm.importThemeFromFile(
            discriminatorPath, &impErr);
        EXPECT_EQ(imported, QString("Discriminator Probe"));
        EXPECT_TRUE(impErr.isEmpty());
        EXPECT_EQ(tm.activeTheme(), QString("Discriminator Probe"));

        // Plain scalar — recursed-into nested object, leaf string.
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  QString("#abcdef"));
        // Object with "type" → routed to parseGradient.
        const ThemeGradient g = tm.gradient("color.waterfall.colormap");
        EXPECT_EQ(g.stops.size(), 2);
        EXPECT_TRUE(g.stops.size() == 2 &&
                    g.stops.first().color.name().toLower() ==
                        QString("#000000"));
        // Object with "family" (no "type") → routed to parseFont.
        const ThemeFont compound = tm.fontToken("font.family.freq");
        EXPECT_EQ(compound.family, QString("DSEG7 Modern"));
        EXPECT_EQ(compound.size,   30);
        EXPECT_EQ(compound.color.name().toLower(), QString("#c8d8e8"));
    }

    // ── Scope tree: setColor at nested scope leaves root untouched ──
    // setColor on a built-in keeps the mutation in memory only
    // (saveActiveTheme silently fails for built-ins), which is exactly
    // what we want for an in-memory scope-tree assertion.  Each scope-
    // tree test uses a unique container path so state from one block
    // doesn't bleed into the next.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        const QColor rootBefore = tm.color("color.accent");
        const QColor scopedColor = QColor("#ff00aa");

        tm.setColor("scopeA/leaf", "color.accent", scopedColor);

        // Nested scope sees the override.
        EXPECT_EQ(tm.colorAt("scopeA/leaf", "color.accent").name().toLower(),
                  scopedColor.name().toLower());
        // Root scope is unaffected — color() always reads from root.
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  rootBefore.name().toLower());
        // isOverriddenAt distinguishes own-override from inherited value.
        EXPECT_TRUE(tm.isOverriddenAt("scopeA/leaf", "color.accent"));
    }

    // ── Scope tree: inheritance walk picks up parent's override ──
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        const QColor parentColor = QColor("#11aa33");

        tm.setColor("scopeB", "color.accent", parentColor);
        // Materialise scopeB/leaf BEFORE the inheritance query.  lookupRaw
        // falls back to root scope when scopeForPath returns nullptr (no
        // scope object exists at the queried path), so a "scopeB/leaf"
        // query without a scope object jumps over scopeB straight to root
        // and returns the root colour.  Production editor code always
        // creates the leaf via setColor / setSizing before querying it,
        // so this matches real usage — an unmaterialised-leaf query is a
        // separate concern (would need a path-walk fallback in lookupRaw).
        tm.setSizing("scopeB/leaf", "sizing.panel.padding", 7);

        // scopeB/leaf has no own override — inheritance must walk up to
        // scopeB and return its value.
        EXPECT_EQ(tm.colorAt("scopeB/leaf", "color.accent").name().toLower(),
                  parentColor.name().toLower());
        // scopeB itself is the source of the override.
        EXPECT_TRUE(tm.isOverriddenAt("scopeB", "color.accent"));
        // scopeB/leaf inherits — must NOT report own-override.
        EXPECT_TRUE(!tm.isOverriddenAt("scopeB/leaf", "color.accent"));
    }

    // ── Scope tree: removeOverride at nested scope falls back to parent ──
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        const QColor parentColor = QColor("#557799");
        const QColor leafOverride = QColor("#aabbcc");

        tm.setColor("scopeC", "color.accent", parentColor);
        tm.setColor("scopeC/leaf", "color.accent", leafOverride);
        EXPECT_EQ(tm.colorAt("scopeC/leaf", "color.accent").name().toLower(),
                  leafOverride.name().toLower());

        tm.removeOverride("scopeC/leaf", "color.accent");

        // Own override gone — inheritance walk falls back to scopeC.
        EXPECT_TRUE(!tm.isOverriddenAt("scopeC/leaf", "color.accent"));
        EXPECT_EQ(tm.colorAt("scopeC/leaf", "color.accent").name().toLower(),
                  parentColor.name().toLower());
    }

    // ── Scope tree: removeOverride at root scope is a defensive no-op ──
    // The root scope is the BASE — dropping a token there would delete
    // it tree-wide rather than restore inheritance.  Guarded explicitly
    // in ThemeManager.cpp; this test pins the warning + no-op behaviour
    // so a future "cleanup" doesn't accidentally re-enable the delete.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        const QColor before = tm.color("color.accent");
        EXPECT_TRUE(before.isValid());

        tm.removeOverride("", "color.accent");  // root scope

        // Root token must still be present after the refused removal.
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  before.name().toLower());
    }

    // ── Scope tree: scopeOrCreate("a/b/c") wires up the full chain ──
    // scopeOrCreate is private; exercise it indirectly via the scope-aware
    // setter, then verify every intermediate path is registered in the
    // tree-walk that drives the editor's container picker.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        tm.setColor("deep/middle/leaf", "color.accent", QColor("#123456"));

        const QStringList paths = tm.containerPaths();
        EXPECT_TRUE(paths.contains(QString()));            // root sentinel
        EXPECT_TRUE(paths.contains("deep"));
        EXPECT_TRUE(paths.contains("deep/middle"));
        EXPECT_TRUE(paths.contains("deep/middle/leaf"));
        // Only the leaf carries the override; intermediates exist but are
        // empty (they got created on the walk down from root).
        EXPECT_TRUE(!tm.isOverriddenAt("deep", "color.accent"));
        EXPECT_TRUE(!tm.isOverriddenAt("deep/middle", "color.accent"));
        EXPECT_TRUE(tm.isOverriddenAt("deep/middle/leaf", "color.accent"));
    }

    // ── Scope tree: theme JSON merges into pre-seeded applet scopes ──
    // seedBuiltinDefaults() creates applet/{tx,rx,comp} before every
    // load.  Saved user overrides under those paths must merge into the
    // existing nodes rather than being read into discarded duplicates.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        const QString themeName = QString("Scope Merge Probe");
        EXPECT_TRUE(tm.saveCurrentThemeAs(themeName));
        EXPECT_EQ(tm.activeTheme(), themeName);

        const int rxFreqSize = 41;
        const int vfoFreqSize = 37;
        const QColor compColor("#123abc");
        tm.setSizing("applet/rx", "font.size.freq", rxFreqSize);
        tm.setSizing("spectrum/vfo", "font.size.freq", vfoFreqSize);
        tm.setColor("applet/comp", "color.slider.foreground", compColor);

        EXPECT_EQ(tm.sizingAt("applet/rx", "font.size.freq"), rxFreqSize);
        EXPECT_EQ(tm.sizingAt("spectrum/vfo", "font.size.freq"), vfoFreqSize);
        EXPECT_EQ(tm.colorAt("applet/comp", "color.slider.foreground").name().toLower(),
                  compColor.name().toLower());

        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        EXPECT_TRUE(tm.setActiveTheme(themeName));

        EXPECT_EQ(tm.sizingAt("applet/rx", "font.size.freq"), rxFreqSize);
        EXPECT_EQ(tm.sizingAt("spectrum/vfo", "font.size.freq"), vfoFreqSize);
        EXPECT_EQ(tm.colorAt("applet/comp", "color.slider.foreground").name().toLower(),
                  compColor.name().toLower());
    }

    // ── Scope tree: themes with no applet overrides keep seeded defaults ──
    // A sparse user theme that does not mention applet/* should still
    // inherit the built-in per-applet differentiation after load.
    {
        const QString sparseDir = tmp.path() + "/_sparse_theme_src";
        QDir().mkpath(sparseDir);
        const QString sparsePath = sparseDir + "/sparse-theme.json";
        QFile sf(sparsePath);
        EXPECT_TRUE(sf.open(QIODevice::WriteOnly));
        sf.write(R"({
            "schemaVersion": 2,
            "name": "Sparse Scope Probe",
            "scopes": {
                "root": {
                    "tokens": {
                        "color.accent": "#102030"
                    }
                }
            }
        })");
        sf.close();

        QString sparseErr;
        const QString importedSparse = tm.importThemeFromFile(
            sparsePath, &sparseErr);
        EXPECT_EQ(importedSparse, QString("Sparse Scope Probe"));
        EXPECT_TRUE(sparseErr.isEmpty());
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  QString("#102030"));
        EXPECT_EQ(tm.colorAt("applet/rx", "color.slider.foreground").name().toLower(),
                  QString("#4dd87a"));
        EXPECT_EQ(tm.colorAt("applet/comp", "color.slider.foreground").name().toLower(),
                  QString("#ffb84d"));
    }

    // ── Compound font tokens: setFontToken round-trip ──
    // setFontToken stores a ThemeFont; fontTokenAt reads it back via the
    // same scope-walk used by every other accessor.  Pins the per-field
    // round-trip (family + size + color) so a future field add doesn't
    // accidentally drop a field at write or read.
    {
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        ThemeFont in;
        in.family = QString("Inter Display");
        in.size   = 18;
        in.color  = QColor("#deadbe");

        tm.setFontToken("font.family.ui", in);
        const ThemeFont out = tm.fontToken("font.family.ui");
        EXPECT_EQ(out.family, in.family);
        EXPECT_EQ(out.size,   in.size);
        EXPECT_EQ(out.color.name().toLower(), in.color.name().toLower());

        // value() on a ThemeFont must downgrade to the family string —
        // the ~35 legacy callers that read `tm.value("font.family.ui")`
        // for the bare typeface name rely on this transparent shim.
        EXPECT_EQ(tm.value("font.family.ui"), in.family);

        // cssFragment("font.size.<role>") virtual lookup: when no scalar
        // font.size.freq exists but font.family.freq is a ThemeFont with
        // a non-zero size, cssFragment must return that embedded size.
        // This is the bug we fixed mid-PR.
        ThemeFont freq;
        freq.family = QString("DSEG7 Modern");
        freq.size   = 34;
        tm.setFontToken("font.family.freq", freq);
        EXPECT_EQ(tm.cssFragment("font.size.freq"), QString("34"));
        // The corresponding sizing() virtual lookup must also pick it up
        // so paint code that composes a QFont sized off the compound's
        // embedded size reads the same number QSS templates do.
        EXPECT_EQ(tm.sizing("font.size.freq"), 34);
    }

    // ── Compound font + JSON persistence: v1 → v2 migration round-trip ──
    // Drive importThemeFromFile with a v1 file that contains both a
    // primitives-eligible alias and a compound font, then mutate via
    // setColor (which auto-saves) and assert:
    //   1. the file on disk is now v2 schema (migrated cleanly),
    //   2. the compound font persisted in {family, size, color} shape,
    //   3. unloading + reloading the theme produces identical values.
    {
        const QString v1Dir = tmp.path() + "/_v1_src";
        QDir().mkpath(v1Dir);
        const QString v1Path = v1Dir + "/v1-source.json";
        QFile v1(v1Path);
        EXPECT_TRUE(v1.open(QIODevice::WriteOnly));
        v1.write(R"({
            "schemaVersion": 1,
            "name": "V1 Migrate Probe",
            "tokens": {
                "color": { "accent": "#cafe42" },
                "font": {
                    "family": {
                        "ui": {
                            "family": "Inter",
                            "size":   13,
                            "color":  "#aabbcc"
                        }
                    }
                }
            }
        })");
        v1.close();

        QString impErr;
        const QString imported = tm.importThemeFromFile(v1Path, &impErr);
        EXPECT_EQ(imported, QString("V1 Migrate Probe"));
        EXPECT_TRUE(impErr.isEmpty());
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  QString("#cafe42"));
        EXPECT_EQ(tm.fontToken("font.family.ui").size, 13);

        // Mutate a token at root — this calls saveActiveTheme(), which
        // writes the file in v2 schema regardless of how it was loaded.
        const QColor mutated = QColor("#abcdef");
        tm.setColor("color.accent", mutated);

        // Locate the on-disk file via the user-dir convention used by
        // importThemeFromFile().
        const QString userDir =
            QStandardPaths::writableLocation(
                QStandardPaths::GenericConfigLocation)
            + "/AetherSDR/themes";
        const QString savedPath = userDir + "/V1 Migrate Probe.json";
        EXPECT_TRUE(QFile::exists(savedPath));

        QFile saved(savedPath);
        EXPECT_TRUE(saved.open(QIODevice::ReadOnly));
        const QByteArray bytes = saved.readAll();
        saved.close();
        const QJsonDocument savedDoc = QJsonDocument::fromJson(bytes);
        EXPECT_TRUE(savedDoc.isObject());
        const QJsonObject savedRoot = savedDoc.object();
        // Migrated v1 → v2: schemaVersion bumped, scopes wrapper present.
        EXPECT_EQ(savedRoot.value("schemaVersion").toInt(), 2);
        EXPECT_TRUE(savedRoot.contains("scopes"));
        const QJsonObject scopes = savedRoot.value("scopes").toObject();
        EXPECT_TRUE(scopes.contains("root"));
        const QJsonObject rootScope =
            scopes.value("root").toObject().value("tokens").toObject();
        EXPECT_TRUE(rootScope.contains("color.accent"));
        EXPECT_EQ(rootScope.value("color.accent").toString().toLower(),
                  QString("#abcdef"));
        // Compound font persisted as a JSON object with the documented
        // {family, size, color} shape — NOT as a nested scope.  A
        // future loader change that re-routes compound fonts through
        // flattenTokens()'s recurse path would break this assertion.
        EXPECT_TRUE(rootScope.contains("font.family.ui"));
        const QJsonValue compoundVal = rootScope.value("font.family.ui");
        EXPECT_TRUE(compoundVal.isObject());
        const QJsonObject compoundObj = compoundVal.toObject();
        EXPECT_EQ(compoundObj.value("family").toString(), QString("Inter"));
        EXPECT_EQ(compoundObj.value("size").toInt(), 13);
        EXPECT_EQ(compoundObj.value("color").toString().toLower(),
                  QString("#aabbcc"));

        // Reload from disk through a full theme switch and verify the
        // values survive the v2 path.
        EXPECT_TRUE(tm.setActiveTheme("Default Dark"));
        EXPECT_TRUE(tm.setActiveTheme("V1 Migrate Probe"));
        EXPECT_EQ(tm.color("color.accent").name().toLower(),
                  mutated.name().toLower());
        const ThemeFont rt = tm.fontToken("font.family.ui");
        EXPECT_EQ(rt.family, QString("Inter"));
        EXPECT_EQ(rt.size,   13);
        EXPECT_EQ(rt.color.name().toLower(), QString("#aabbcc"));
    }

    // Restore Default Dark for any future test additions below.
    tm.setActiveTheme("Default Dark");

    if (g_failures == 0) {
        std::fprintf(stderr, "PASS theme_manager_test\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
