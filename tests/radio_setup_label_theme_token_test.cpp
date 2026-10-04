// #5896 — RadioSetupDialog's caption labels and line edits.
//
// `kLabelStyle` pinned `#c8d8e8` and `kEditStyle` pinned `#1a2a3a` / `#304050` /
// `#c8d8e8`, and both were painted with a plain `QWidget::setStyleSheet`. Only
// `ThemeManager::applyStyleSheet` records a widget in `m_trackedWidgets` for
// re-resolution on `themeChanged`, so the literals survived View ▸ Theme
// unchanged: under Default Light the captions rendered Default Dark's
// `text.primary` on a `#f5f5f8` ground, and the line edits kept a near-black
// fill.
//
// THE SWITCH IS THE ONLY DISCRIMINATOR, and that is the whole shape of this
// file. Every literal this change removed IS the value its replacement token
// resolves to under Default Dark — that exactness is the reason the conversion
// is safe, and it is also the reason a construction-time assertion cannot see
// the defect: before and after, a freshly built dialog under Default Dark is
// byte-identical. A test that only inspected a new widget would pass against
// the broken tree. So the colour is read back ACROSS a theme switch, and the
// dark→light→dark round trip is asserted rather than a single reading.
//
// Nothing is collected by name. Widgets are gathered by the STYLE SHAPE with
// the colour left open — `QLabel { color: <any hex or resolved value>;
// font-size: 12px; }` — so the same collector runs against both trees, and a
// named floor then pins that the collection is not empty and not accidentally
// narrowed to the one label that happens to be right.
//
// No hardware and no transport: a bare `RadioModel`, never connected. Only the
// Radio page is built eagerly (#1776 keeps the hardware probes out of the
// constructor), and that page alone carries the three line edits and the
// makeInfoField captions the floor below names, so no deferred page has to be
// opened and no probe is triggered.
//
// AND THAT LAZINESS IS LOAD-BEARING FOR THE COLLECTOR, not just for the probes.
// `captionShape()` leaves the colour open, so it matches an UNTRACKED literal
// just as happily as a resolved token — and the registration loop then demands
// `color.text.primary` in `tokensForWidget()` for everything it collected. An
// untracked caption of that exact shape on the EAGER page would therefore fail
// this test for a reason that has nothing to do with the defect it pins.
//
// Four untracked labels of that exact shape survive in `RadioSetupDialog.cpp`,
// and all four sit on deferred pages:
//
//   - `buildRxTab`, Frequency Offset group — the GPSDO-present note, both
//     branches (`#00c040` / `#c0a000`).
//   - `buildUsbCablesTab` — the "No USB cables detected." placeholder and the
//     "Select a cable type to configure this device." note (`#606880`, twice).
//
// Inside `buildRadioTab` every surviving raw-hex label is 10px or 11px — the
// `m_fwStatusLabel` group (#5896's stated deferral: `#6888a0` is not any
// token's Default Dark value) at 10px, and the licence note at 11px — so none
// of them is collected. Make another page eager, or move one of those four
// labels onto the Radio page, and this test goes red with nothing broken. The
// fix then is to route that label through `applyLabelStyle`, NOT to loosen the
// regex: narrowing the collector to dodge a real untracked widget is how this
// test would start agreeing with itself.

#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QtTest>

using namespace AetherSDR;

namespace {

const char* const kDarkTheme  = "Default Dark";
const char* const kLightTheme = "Default Light";

// Colour left open on purpose: the same regex has to match the literal the
// broken tree pastes in AND the value the token resolves to. Anchored, so a
// value label (`font-weight: bold`) or the 11px serial-tab caption cannot drift
// into the set.
const QRegularExpression& captionShape()
{
    static const QRegularExpression re(
        QStringLiteral("^QLabel \\{ color: #[0-9a-fA-F]{6}; font-size: 12px; \\}$"));
    return re;
}

const QRegularExpression& editShape()
{
    static const QRegularExpression re(
        QStringLiteral("^QLineEdit \\{ background: #[0-9a-fA-F]{6}; "
                       "border: 1px solid #[0-9a-fA-F]{6}; border-radius: 3px; "
                       "color: #[0-9a-fA-F]{6}; font-size: 12px; padding: 2px 4px; \\}$"));
    return re;
}

QList<QLabel*> captionLabels(QWidget* root)
{
    QList<QLabel*> out;
    for (QLabel* l : root->findChildren<QLabel*>()) {
        if (captionShape().match(l->styleSheet()).hasMatch())
            out.append(l);
    }
    return out;
}

QList<QLineEdit*> styledEdits(QWidget* root)
{
    QList<QLineEdit*> out;
    for (QLineEdit* e : root->findChildren<QLineEdit*>()) {
        if (editShape().match(e->styleSheet()).hasMatch())
            out.append(e);
    }
    return out;
}

} // namespace

class RadioSetupLabelThemeTokenTest : public QObject {
    Q_OBJECT
private slots:

    // CONTROL FOR EVERYTHING BELOW. Without :/themes/ compiled into this
    // target, ThemeSeedGenerated.cpp still resolves every token, so a
    // construction-time reading succeeds — but availableThemes() is empty,
    // setActiveTheme() has nothing to switch to, and every "the colour moved"
    // assertion would be comparing a value against itself. That failure passes
    // silently, which is the same class of defect this file is about one level
    // up, so it is asserted first and on its own.
    void theThemesAreReachableAndDisagree()
    {
        auto& tm = ThemeManager::instance();
        const QStringList themes = tm.availableThemes();
        QVERIFY2(themes.contains(QLatin1String(kDarkTheme)), qPrintable(themes.join(", ")));
        QVERIFY2(themes.contains(QLatin1String(kLightTheme)), qPrintable(themes.join(", ")));

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
        const QColor darkText = tm.color(QStringLiteral("color.text.primary"));
        const QColor darkBg1  = tm.color(QStringLiteral("color.background.1"));
        const QColor darkBg2  = tm.color(QStringLiteral("color.background.2"));
        QVERIFY(tm.setActiveTheme(QLatin1String(kLightTheme)));
        const QColor lightText = tm.color(QStringLiteral("color.text.primary"));
        const QColor lightBg1  = tm.color(QStringLiteral("color.background.1"));
        const QColor lightBg2  = tm.color(QStringLiteral("color.background.2"));

        QVERIFY(darkText.isValid() && lightText.isValid());
        // If the two themes ever agreed about a token, "the colour followed the
        // switch" would be unobservable and the slots below would pass without
        // measuring anything.
        QVERIFY(darkText != lightText);
        QVERIFY(darkBg1 != lightBg1);
        QVERIFY(darkBg2 != lightBg2);

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }

    // The equivalence claim this change rests on, pinned rather than asserted in
    // a commit message: each literal removed from RadioSetupDialog.cpp IS the
    // value its replacement token resolves to under Default Dark, so Radio
    // Setup's dark appearance did not move. The hex below is the value the
    // dialog USED to carry; it no longer appears in the production file for
    // these constants, so this cannot agree with itself.
    //
    // If this ever fails, Default Dark's palette has been retuned and Radio
    // Setup's dark appearance has moved with it. That is a decision someone
    // made, not a bug in this test — but it should be a visible one.
    void theTokensResolveToExactlyTheLiteralsTheyReplaced()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
        QCOMPARE(tm.color(QStringLiteral("color.text.primary")).name(),
                 QStringLiteral("#c8d8e8"));   // was kLabelStyle / kEditStyle's colour
        QCOMPARE(tm.color(QStringLiteral("color.background.1")).name(),
                 QStringLiteral("#1a2a3a"));   // was kEditStyle's background
        QCOMPARE(tm.color(QStringLiteral("color.background.2")).name(),
                 QStringLiteral("#304050"));   // was kEditStyle's border
    }

    // The named floor. A shape collector that matched nothing would make every
    // "for each collected widget" loop below vacuously true, and one that
    // matched only the labels that were already right would be worse than
    // empty. These captions and these edits are on the eagerly built Radio page
    // and are named one by one.
    void theCollectionHasANamedFloor()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        QStringList texts;
        for (QLabel* l : captionLabels(&dialog))
            texts << l->text();
        for (const char* wanted : {"Nickname:", "Callsign:", "Station Name:"})
            QVERIFY2(texts.contains(QLatin1String(wanted)),
                     qPrintable(QStringLiteral("%1 missing from: %2")
                                .arg(QLatin1String(wanted), texts.join(QStringLiteral(" | ")))));
        QVERIFY2(texts.size() >= 8, qPrintable(QString::number(texts.size())));

        QStringList editNames;
        for (QLineEdit* e : styledEdits(&dialog))
            editNames << e->accessibleName();
        for (const char* wanted : {"Radio nickname", "Station callsign"})
            QVERIFY2(editNames.contains(QLatin1String(wanted)),
                     qPrintable(editNames.join(QStringLiteral(" | "))));
        QVERIFY2(editNames.size() >= 3, qPrintable(QString::number(editNames.size())));
    }

    // THE SLOT THE FIX IS FOR. Dark → Light → Dark, reading the painted colour
    // back each time. Against the pre-#5896 tree the first pass passes (the
    // literal IS the dark value) and the Light pass fails on every caption.
    void captionLabelColoursFollowAThemeSwitch()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        const QList<QLabel*> labels = captionLabels(&dialog);
        QVERIFY(!labels.isEmpty());

        // Registration. A widget painted with a plain setStyleSheet is not in
        // m_trackedWidgets at all, so this is the construction-time half of the
        // defect and it fails on the broken tree without any switch.
        for (QLabel* l : labels) {
            QVERIFY2(tm.tokensForWidget(l).contains(QStringLiteral("color.text.primary")),
                     qPrintable(QStringLiteral("not registered against the token: %1 | qss=%2")
                                .arg(l->text(), l->styleSheet())));
        }

        auto assertPainted = [&labels](const char* theme, const QColor& expected) {
            auto& tm = ThemeManager::instance();
            QVERIFY(tm.setActiveTheme(QLatin1String(theme)));
            const QString want = expected.name();
            for (QLabel* l : labels) {
                QVERIFY2(l->styleSheet().contains(want, Qt::CaseInsensitive),
                         qPrintable(QStringLiteral("expected %1 under %2: %3 | qss=%4")
                                    .arg(want, QLatin1String(theme), l->text(), l->styleSheet())));
            }
        };

        QVERIFY(tm.setActiveTheme(QLatin1String(kLightTheme)));
        const QColor light = tm.color(QStringLiteral("color.text.primary"));
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
        const QColor dark = tm.color(QStringLiteral("color.text.primary"));

        assertPainted(kDarkTheme, dark);
        assertPainted(kLightTheme, light);   // the fix
        assertPainted(kDarkTheme, dark);     // and back, so it is not a one-way latch

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }

    // The same, for the line edits. Three tokens rather than one, and the
    // background is the one an operator sees: #1a2a3a under a light theme is a
    // near-black box, and it is simultaneously Default Light's text.primary, so
    // the text the theme did reach landed black on black.
    void lineEditColoursFollowAThemeSwitch()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        const QList<QLineEdit*> edits = styledEdits(&dialog);
        QVERIFY(!edits.isEmpty());

        for (QLineEdit* e : edits) {
            const QStringList tokens = tm.tokensForWidget(e);
            for (const char* token : {"color.background.1", "color.background.2",
                                      "color.text.primary"}) {
                QVERIFY2(tokens.contains(QLatin1String(token)),
                         qPrintable(QStringLiteral("%1 not registered on %2 | qss=%3")
                                    .arg(QLatin1String(token), e->accessibleName(),
                                         e->styleSheet())));
            }
        }

        auto assertPainted = [&edits](const char* theme) {
            auto& tm = ThemeManager::instance();
            QVERIFY(tm.setActiveTheme(QLatin1String(theme)));
            const QStringList want{
                tm.color(QStringLiteral("color.background.1")).name(),
                tm.color(QStringLiteral("color.background.2")).name(),
                tm.color(QStringLiteral("color.text.primary")).name()};
            for (QLineEdit* e : edits) {
                for (const QString& value : want) {
                    QVERIFY2(e->styleSheet().contains(value, Qt::CaseInsensitive),
                             qPrintable(QStringLiteral("expected %1 under %2: %3 | qss=%4")
                                        .arg(value, QLatin1String(theme),
                                             e->accessibleName(), e->styleSheet())));
                }
            }
        };

        assertPainted(kDarkTheme);
        assertPainted(kLightTheme);   // the fix
        assertPainted(kDarkTheme);

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-label-theme-token"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupLabelThemeTokenTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_label_theme_token_test.moc"
