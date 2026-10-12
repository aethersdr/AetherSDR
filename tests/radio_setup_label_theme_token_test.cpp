// #5896 — Radio Setup's caption labels and line edits follow a LIVE theme switch.
//
// Radio Setup is a canon window: its body carries one sheet built from
// color.canon.* tokens, re-applied on themeChanged (applyCanonSheet), and a
// caption is a QLabel with the Caption tone (`rsTone`) and no sheet of its
// own. A widget that kept its own sheet — a literal, or a token template that
// pins a color — would override the window's and could survive View ▸ Theme
// unchanged, which is the defect: under Default Light the captions once
// rendered Default Dark's text on a light ground, and the line edits kept a
// near-black fill.
//
// THE SWITCH IS THE ONLY DISCRIMINATOR. A freshly built dialog under Default
// Dark looks right whether or not its colors can move, so every color here is
// read back from the polished palette ACROSS a theme switch, and the
// dark→light→dark round trip is asserted rather than a single reading.
//
// Nothing is collected by name. Captions are gathered by their role and line
// edits by type, and a named floor then pins that the collection is not empty
// and not accidentally narrowed.
//
// No hardware and no transport: a bare `RadioModel`, never connected. Only the
// Radio page is built eagerly (#1776 keeps the hardware probes out of the
// constructor), and that page alone carries the line edits and captions the
// floor below names, so no deferred page has to be opened and no probe runs.
// A tone changed on a live label (a status line) repaints at once.

#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "gui/RadioSetupDialog.h"
#include "gui/RadioSetupDialogCommon.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QtTest>

using namespace AetherSDR;

namespace {

const char* const kDarkTheme  = "Default Dark";
const char* const kLightTheme = "Default Light";

QList<QLabel*> captionLabels(QWidget* root)
{
    QList<QLabel*> out;
    for (QLabel* l : root->findChildren<QLabel*>()) {
        if (l->property("rsTone").toByteArray() == "caption")
            out.append(l);
    }
    return out;
}

// The page's own fields: the search box above the navigation has a sheet rule
// of its own and is not a settings field.
QList<QLineEdit*> fieldEdits(QWidget* root)
{
    QList<QLineEdit*> out;
    for (QLineEdit* e : root->findChildren<QLineEdit*>()) {
        if (e->objectName() != QLatin1String("radioSetupSearch") && e->isVisibleTo(root))
            out.append(e);
    }
    return out;
}

QColor polished(QWidget* w, QPalette::ColorRole role)
{
    w->ensurePolished();
    return w->palette().color(role);
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
    // silently, so it is asserted first and on its own.
    void theThemesAreReachableAndDisagree()
    {
        auto& tm = ThemeManager::instance();
        const QStringList themes = tm.availableThemes();
        QVERIFY2(themes.contains(QLatin1String(kDarkTheme)), qPrintable(themes.join(", ")));
        QVERIFY2(themes.contains(QLatin1String(kLightTheme)), qPrintable(themes.join(", ")));

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
        const QColor darkCaption = tm.color(QStringLiteral("color.canon.inkSoft"));
        const QColor darkField   = tm.color(QStringLiteral("color.canon.control"));
        const QColor darkInk     = tm.color(QStringLiteral("color.canon.ink"));
        QVERIFY(tm.setActiveTheme(QLatin1String(kLightTheme)));
        const QColor lightCaption = tm.color(QStringLiteral("color.canon.inkSoft"));
        const QColor lightField   = tm.color(QStringLiteral("color.canon.control"));
        const QColor lightInk     = tm.color(QStringLiteral("color.canon.ink"));

        QVERIFY(darkCaption.isValid() && lightCaption.isValid());
        // If the two themes ever agreed about a token, "the colour followed the
        // switch" would be unobservable and the slots below would pass without
        // measuring anything.
        QVERIFY(darkCaption != lightCaption);
        QVERIFY(darkField != lightField);
        QVERIFY(darkInk != lightInk);

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }

    // The named floor. A collector that matched nothing would make every "for
    // each collected widget" loop below vacuously true. These captions and
    // these edits are on the eagerly built Radio page and are named one by one.
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
        for (QLineEdit* e : fieldEdits(&dialog))
            editNames << e->accessibleName();
        for (const char* wanted : {"Radio nickname", "Station callsign"})
            QVERIFY2(editNames.contains(QLatin1String(wanted)),
                     qPrintable(editNames.join(QStringLiteral(" | "))));
        QVERIFY2(editNames.size() >= 3, qPrintable(QString::number(editNames.size())));
    }

    // No caption and no field carries a sheet of its own: one would override
    // the window's, and a color it pinned would not follow the switch.
    void captionsAndFieldsTakeTheWindowSheet()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        for (QLabel* l : captionLabels(&dialog))
            QVERIFY2(l->styleSheet().isEmpty(),
                     qPrintable(QStringLiteral("%1 | qss=%2").arg(l->text(), l->styleSheet())));
        for (QLineEdit* e : fieldEdits(&dialog))
            QVERIFY2(e->styleSheet().isEmpty(),
                     qPrintable(QStringLiteral("%1 | qss=%2")
                                .arg(e->accessibleName(), e->styleSheet())));
    }

    // THE SLOT THE FIX IS FOR. Dark → Light → Dark, reading the painted colour
    // back each time.
    void captionLabelColoursFollowAThemeSwitch()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        const QList<QLabel*> labels = captionLabels(&dialog);
        QVERIFY(!labels.isEmpty());

        auto assertPainted = [&labels](const char* theme) {
            auto& tm = ThemeManager::instance();
            QVERIFY(tm.setActiveTheme(QLatin1String(theme)));
            const QColor want = tm.color(QStringLiteral("color.canon.inkSoft"));
            for (QLabel* l : labels) {
                const QColor got = polished(l, QPalette::WindowText);
                QVERIFY2(got == want,
                         qPrintable(QStringLiteral("expected %1 under %2: %3 is %4")
                                    .arg(want.name(), QLatin1String(theme), l->text(),
                                         got.name())));
            }
        };

        assertPainted(kDarkTheme);
        assertPainted(kLightTheme);   // the fix
        assertPainted(kDarkTheme);    // and back, so it is not a one-way latch

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }

    // A status line changes tone while the window is open (an upload that
    // fails, a calibration that settles). setTone re-polishes the live widget,
    // so the new color is painted at once rather than on the next theme
    // change or reopen.
    void aToneChangedOnAShownLabelRepaints()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        const QList<QLabel*> labels = captionLabels(&dialog);
        QVERIFY(!labels.isEmpty());
        QLabel* label = labels.first();
        const QColor caption = tm.color(QStringLiteral("color.canon.inkSoft"));
        const QColor error   = tm.color(QStringLiteral("color.accent.danger"));
        const QColor ok      = tm.color(QStringLiteral("color.accent.success"));
        // CONTROL: the three colors are distinguishable.
        QVERIFY(caption != error && error != ok && caption != ok);
        QCOMPARE(polished(label, QPalette::WindowText), caption);

        setTone(label, Tone::Error);
        QCOMPARE(polished(label, QPalette::WindowText), error);
        setTone(label, Tone::Ok);
        QCOMPARE(polished(label, QPalette::WindowText), ok);
        setTone(label, Tone::Caption);
        QCOMPARE(polished(label, QPalette::WindowText), caption);
    }

    // The same, for the line edits: the fill an operator sees, and the text on
    // it.
    void lineEditColoursFollowAThemeSwitch()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        const QList<QLineEdit*> edits = fieldEdits(&dialog);
        QVERIFY(!edits.isEmpty());

        auto assertPainted = [&edits](const char* theme) {
            auto& tm = ThemeManager::instance();
            QVERIFY(tm.setActiveTheme(QLatin1String(theme)));
            const QColor fill = tm.color(QStringLiteral("color.canon.control"));
            const QColor ink  = tm.color(QStringLiteral("color.canon.ink"));
            int checked = 0;
            for (QLineEdit* e : edits) {
                // A read-only field sits nested rather than on control.
                if (e->isReadOnly() || !e->isEnabled())
                    continue;
                ++checked;
                const QColor gotFill = polished(e, QPalette::Base);
                const QColor gotInk  = polished(e, QPalette::Text);
                QVERIFY2(gotFill == fill && gotInk == ink,
                         qPrintable(QStringLiteral("expected %1 on %2 under %3: %4 is %5 on %6")
                                    .arg(ink.name(), fill.name(), QLatin1String(theme),
                                         e->accessibleName(), gotInk.name(), gotFill.name())));
            }
            QVERIFY2(checked >= 2, qPrintable(QString::number(checked)));
        };

        assertPainted(kDarkTheme);
        assertPainted(kLightTheme);   // the fix
        assertPainted(kDarkTheme);

        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));
    }

    // A USB cable's list color is a resolved token, so it is re-resolved on a
    // switch rather than kept from the theme the list was filled under.
    void usbCableColorsFollowAThemeSwitch()
    {
        auto& tm = ThemeManager::instance();
        QVERIFY(tm.setActiveTheme(QLatin1String(kDarkTheme)));

        RadioModel model;
        // A disabled CAT cable, injected as the radio's status would report it.
        model.usbCableModel().applyStatus(QStringLiteral("SNTHEME1"),
                                          {{QStringLiteral("type"), QStringLiteral("cat")},
                                           {QStringLiteral("enable"), QStringLiteral("0")},
                                           {QStringLiteral("plugged_in"), QStringLiteral("1")}});
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("USB Cables"));
        dialog.show();

        QListWidgetItem* cable = nullptr;
        for (QListWidget* list : dialog.findChildren<QListWidget*>()) {
            for (int i = 0; i < list->count() && !cable; ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == QLatin1String("SNTHEME1"))
                    cable = list->item(i);
            }
        }
        QVERIFY(cable);

        for (const char* theme : {kDarkTheme, kLightTheme, kDarkTheme}) {
            QVERIFY(tm.setActiveTheme(QLatin1String(theme)));
            QCOMPARE(cable->foreground().color(), tm.color(QStringLiteral("color.canon.muted")));
        }
    }

    // Radio Side and Client Side are one choice between two: both take the
    // Select role, so whichever is chosen reads the same.
    void recordModeButtonsAreOneSelectSet()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("Audio"));

        QStringList roles;
        for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("Radio Side") || b->text() == QLatin1String("Client Side"))
                roles << b->text() + QLatin1Char('=') + b->property("rsButton").toString();
        }
        roles.sort();
        QCOMPARE(roles, QStringList({QStringLiteral("Client Side=select"),
                                     QStringLiteral("Radio Side=select")}));
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
