// Pins ThemeManager's COMPILED-IN seed — the table generated from
// resources/themes/default-dark.json by tools/gen_theme_seed.py (#3184).
//
// WHY THIS TARGET EXISTS SEPARATELY FROM theme_manager_test
// ---------------------------------------------------------
// theme_manager_test links resources.qrc, so ThemeManager's constructor loads
// Default Dark immediately after seeding and the JSON overwrites every token
// the seed set.  A wrong seed is invisible from there — which is precisely how
// nine tokens drifted for months without anyone noticing.
//
// This binary deliberately does NOT link the theme resource.  scanAvailableThemes()
// finds nothing, setActiveTheme() fails, and the seed is what the UI renders —
// the resource-missing fallback path #3184 called out.  Every assertion below
// therefore reads the seed through the ordinary public API.
//
// MUTATION-TESTED, per the house rule that a guard you haven't watched fail is
// not a guard.  Against the pre-#3184 hand-written table these fail as:
//
//   color.slice.a                       #ff4040  (want #00d4ff)
//   color.accent.dim                    #0090e0  (want #0070c0)
//   waterfall.colormap.fire stop count  0        (want > 1 — never seeded)
//
// The CI gate (tools/check_theme_seed.py) proves the GENERATOR agrees with the
// JSON.  It cannot prove the emitted C++ reaches m_tokens at runtime: a target
// that omits ThemeSeedGenerated.cpp from its source list, or a regression in
// seedScopedToken(), passes the gate and ships a transparent UI.  That runtime
// link is what this test covers.

#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>
#include <QWidget>
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

#define EXPECT_COLOR(actual, expectedHex) do { \
    const QColor got_ = (actual); \
    if (!got_.isValid() || got_ != QColor(QStringLiteral(expectedHex))) { \
        std::fprintf(stderr, "FAIL %s:%d  %s = %s, expected %s\n", \
                     __FILE__, __LINE__, #actual, \
                     qPrintable(got_.isValid() ? got_.name() \
                                               : QStringLiteral("<invalid>")), \
                     expectedHex); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-theme-seed-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }

    // QStandardPaths test mode points at a sandbox that PERSISTS between runs
    // (and on Windows is shared across scenarios), so a user theme left behind
    // by another test would be found by scanAvailableThemes() and could load
    // over the seed.  The availableThemes()-is-empty assertion below would catch
    // that, but as a spurious failure in an unrelated PR — so clear the dir
    // before the ThemeManager singleton scans.  Same guard, same reason, as
    // tests/theme_manager_test.cpp.
    {
        const QString sandboxAppDir =
            QStandardPaths::writableLocation(
                QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/AetherSDR");
        QDir(sandboxAppDir).removeRecursively();
    }

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("AetherSDR-test");
    QCoreApplication::setApplicationName("AetherSDR-test");

    auto& tm = ThemeManager::instance();

    // ── Precondition: the seed really is what we are reading ──
    // If this target ever gains the theme resource, every assertion below stops
    // testing the seed and starts testing default-dark.json — silently, and
    // they would all still pass.  Fail loudly instead.
    EXPECT_TRUE(tm.availableThemes().isEmpty());

    // ── The nine tokens that had drifted (#3184) ──
    // The old hand-written table carried an entirely different slice palette.
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.a")), "#00d4ff");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.b")), "#ff40ff");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.c")), "#40ff40");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.d")), "#ffff00");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.e")), "#ffa000");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.f")), "#00e0c0");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.g")), "#ff6080");
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.h")), "#b080ff");
    EXPECT_COLOR(tm.color(QStringLiteral("color.accent.dim")), "#0070c0");

    // ── `{primitive}` aliases are resolved to concrete values ──
    // color.accent.dim above is one.  The seed runs before any primitives
    // palette exists, so a regression that emitted the alias verbatim would
    // leave QColor("{color.blue.700}") — invalid, i.e. transparent.
    EXPECT_COLOR(tm.color(QStringLiteral("color.accent")), "#00b4d8");

    // ── Tokens that previously had NO compiled default at all ──
    // These resolved transparent on any theme predating them, which is worse
    // than a stale value.  One from each of the three scalar families.
    EXPECT_COLOR(tm.color(QStringLiteral("color.slice.dim.a")), "#006080");
    EXPECT_COLOR(tm.color(QStringLiteral("color.highlight.rx")), "#379baf");
    EXPECT_COLOR(tm.color(QStringLiteral("color.button.foreground.disabled")),
                 "#506070");

    // ── Gradient tokens survive the emit → QVariant → ThemeGradient round trip ──
    // All six waterfall colormaps were unseeded before the generator; a missing
    // one gives the operator a blank waterfall with no diagnostic.
    for (const char* map : {"default", "fire", "plasma", "purple",
                            "blueGreen", "grayscale"}) {
        const ThemeGradient g =
            tm.gradient(QStringLiteral("color.waterfall.colormap.")
                        + QLatin1String(map));
        EXPECT_TRUE(g.stops.size() > 1);
        for (const ThemeGradientStop& stop : g.stops)
            EXPECT_TRUE(stop.color.isValid());
    }
    // The meter bar ramp is the other seeded gradient, and the only one that
    // was seeded before the generator — pin it so a generator change that
    // dropped non-colormap gradients is caught too.
    EXPECT_TRUE(tm.gradient(QStringLiteral("color.meter.bar.fillGradient"))
                    .stops.size() > 1);

    // ── Scoped tokens reach the scope tree via seedScopedToken() ──
    // The generated TU cannot see ThemeScope, so it routes nested scopes
    // through that helper.  If the helper regressed, per-applet
    // differentiation would silently collapse to the root value — which is
    // exactly what a bare byte-equality gate cannot see.
    EXPECT_TRUE(tm.containerPaths().contains(QStringLiteral("applet/tx")));
    EXPECT_TRUE(tm.containerPaths().contains(QStringLiteral("applet/rx")));
    EXPECT_TRUE(tm.containerPaths().contains(QStringLiteral("applet/comp")));

    EXPECT_COLOR(tm.color(QStringLiteral("color.slider.foreground")), "#00b4d8");
    {
        QWidget txHost;
        QWidget rxHost;
        theme::setContainer(&txHost, QStringLiteral("applet/tx"));
        theme::setContainer(&rxHost, QStringLiteral("applet/rx"));
        EXPECT_COLOR(tm.color(&txHost, QStringLiteral("color.slider.foreground")),
                     "#ff4d4d");
        EXPECT_COLOR(tm.color(&rxHost, QStringLiteral("color.slider.foreground")),
                     "#4dd87a");
    }

    // ── Non-colour token types make it through ──
    EXPECT_TRUE(tm.sizing(QStringLiteral("sizing.panel.padding")) > 0);
    EXPECT_TRUE(!tm.fontToken(QStringLiteral("font.family.ui")).family.isEmpty());

    if (g_failures == 0) {
        std::fprintf(stdout, "PASS theme_seed_test\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED theme_seed_test (%d failures)\n", g_failures);
    return 1;
}
