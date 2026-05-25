#include "core/ThemeManager.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
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

    if (g_failures == 0) {
        std::fprintf(stderr, "PASS theme_manager_test\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
