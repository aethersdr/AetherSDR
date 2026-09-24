// The AetherModem chrome sheet (src/gui/ModemChrome.cpp) is what gives the
// AetherDSP window its look. Every rule in it is addressed by a selector that
// something else has to keep matching, and when one stops matching Qt says
// nothing at all — the control silently falls back to the generic style and
// only a screenshot shows it. Both failures this test guards against actually
// happened while the window was being built:
//
//   * the tab strip was selected by objectName, and the caller overwrote that
//     objectName with the per-method id the automation bridge needs, so every
//     tab quietly rendered as a plain push button;
//   * the sheet is assembled from 39 positional arguments, and one missing
//     .arg() leaves a literal "%17" in the middle of a declaration, which Qt
//     drops along with the rest of that rule.

#include "gui/ModemChrome.h"
#include "core/ThemeManager.h"

#include "TestSettingsProfile.h"

#include <QApplication>
#include <QRegularExpression>
#include <QString>
#include <QtTest>

#include <cstdio>

using namespace AetherSDR;

class ModemChromeTest : public QObject {
    Q_OBJECT

private slots:
    void bothScalesCarryEveryStructuralSelector();
    void tabsAreSelectedByPropertyNotObjectName();
    void everyPlaceholderIsSubstituted();
    void compactIsSmallerButNotADifferentPalette();
    void everyThemeTokenInTheSheetResolves();
    void checkedLosesToDisabled();
};

namespace {

// First "font-size: Npx" in the sheet — the base QWidget rule, which every
// other size is derived from.
int baseFontSize(const QString& sheet)
{
    static const QRegularExpression re(QStringLiteral("font-size:\\s*(\\d+)px"));
    const auto m = re.match(sheet);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

} // namespace

void ModemChromeTest::bothScalesCarryEveryStructuralSelector()
{
    // The object names ModemChrome.h documents. A page built against this
    // header names its frames and labels with these; a rule that disappears
    // from the sheet takes the panel background or the heading with it.
    const QStringList required = {
        QStringLiteral("QFrame#ControlsFrame"),
        QStringLiteral("QFrame#StatusFrame"),
        QStringLiteral("QFrame#TabsFrame"),
        QStringLiteral("QFrame#ControlCell"),
        QStringLiteral("QLabel#SectionLabel"),
        QStringLiteral("QLabel#StatusValue"),
        QStringLiteral("QLabel#StatusDot"),
        QStringLiteral("QPushButton#IconButton"),
        // A checkable button with no checked rule reads as unlatched however
        // it is set -- which is how BYPASS shipped with no visual feedback.
        QStringLiteral("QPushButton:checked"),
        QStringLiteral("QRadioButton::indicator"),
        QStringLiteral("QCheckBox::indicator"),
        QStringLiteral("QSlider::groove:horizontal"),
        QStringLiteral("QSlider::sub-page:horizontal"),
        QStringLiteral("QSlider::handle:horizontal"),
    };

    for (const auto scale : {ModemChrome::Scale::Dialog, ModemChrome::Scale::Compact}) {
        const QString sheet = ModemChrome::styleSheet(scale);
        for (const QString& selector : required) {
            QVERIFY2(sheet.contains(selector),
                     qPrintable(QStringLiteral("missing %1 at scale %2")
                                    .arg(selector)
                                    .arg(static_cast<int>(scale))));
        }
    }
}

void ModemChromeTest::tabsAreSelectedByPropertyNotObjectName()
{
    for (const auto scale : {ModemChrome::Scale::Dialog, ModemChrome::Scale::Compact}) {
        const QString sheet = ModemChrome::styleSheet(scale);
        // The property selector, and all three states of it. An objectName
        // selector here is the bug: the buttons carry a per-method objectName
        // for the automation bridge, so #TabButton never matches them.
        QVERIFY(sheet.contains(QStringLiteral("QPushButton[chrome=\"tab\"] {")));
        QVERIFY(sheet.contains(QStringLiteral("QPushButton[chrome=\"tab\"]:checked {")));
        QVERIFY(sheet.contains(QStringLiteral("QPushButton[chrome=\"tab\"]:disabled {")));
        QVERIFY2(!sheet.contains(QStringLiteral("QPushButton#TabButton")),
                 "tabs must not be selected by objectName");
    }
}

void ModemChromeTest::everyPlaceholderIsSubstituted()
{
    static const QRegularExpression leftover(QStringLiteral("%\\d"));
    for (const auto scale : {ModemChrome::Scale::Dialog, ModemChrome::Scale::Compact}) {
        const QString sheet = ModemChrome::styleSheet(scale);
        const auto m = leftover.match(sheet);
        QVERIFY2(!m.hasMatch(),
                 qPrintable(QStringLiteral("unsubstituted placeholder %1 near: %2")
                                .arg(m.captured(0))
                                .arg(sheet.mid(qMax(0, m.capturedStart() - 60), 120))));
    }
}

void ModemChromeTest::compactIsSmallerButNotADifferentPalette()
{
    const QString dialog = ModemChrome::styleSheet(ModemChrome::Scale::Dialog);
    const QString compact = ModemChrome::styleSheet(ModemChrome::Scale::Compact);

    QCOMPARE(baseFontSize(dialog), 14);   // the modem's own scale
    QVERIFY(baseFontSize(compact) < baseFontSize(dialog));

    // Only sizes may differ. Two windows that are supposed to look like the
    // same product cannot have one of them drift to another green.
    for (const char* colour : {ModemChrome::Colour::Background,
                               ModemChrome::Colour::Border,
                               ModemChrome::Colour::Green,
                               ModemChrome::Colour::GreenEdge,
                               ModemChrome::Colour::GreenBright,
                               ModemChrome::Colour::Section,
                               ModemChrome::Colour::Field}) {
        const QString hex = QString::fromLatin1(colour);
        QVERIFY2(dialog.contains(hex) && compact.contains(hex), colour);
    }
}

void ModemChromeTest::everyThemeTokenInTheSheetResolves()
{
    // A {{token}} this sheet names but the theme does not define resolves to
    // transparent and paints nothing -- no error, no fallback, just an
    // invisible control. That has already happened once in this codebase
    // (color.accent.ok / color.accent.error, neither of which exists; the
    // real names are success / danger), and it was caught by eye rather than
    // by anything automatic.
    static const QRegularExpression tokenRe(QStringLiteral("\\{\\{([^}]+)\\}\\}"));
    int checked = 0;
    for (const auto scale : {ModemChrome::Scale::Dialog, ModemChrome::Scale::Compact}) {
        const QString sheet = ModemChrome::styleSheet(scale);
        auto it = tokenRe.globalMatch(sheet);
        while (it.hasNext()) {
            const QString token = it.next().captured(1).trimmed();
            const QColor c = AetherSDR::ThemeManager::instance().color(token);
            QVERIFY2(c.isValid() && c.alpha() > 0,
                     qPrintable(QStringLiteral("unresolved theme token: %1").arg(token)));
            ++checked;
        }
    }
    QVERIFY2(checked > 0, "no tokens found -- has the sheet stopped using them?");
}

void ModemChromeTest::checkedLosesToDisabled()
{
    // QPushButton:checked and QPushButton:disabled tie on specificity -- one
    // pseudo-class each on the same type -- so which one paints a button that
    // is both comes down to source order, and the later rule wins. A disabled
    // control must not render as "on and doing something", so :checked has to
    // stay above :disabled. Nothing about that ordering is self-evident when
    // reading the sheet, which is why it is asserted rather than commented.
    for (const auto scale : {ModemChrome::Scale::Dialog, ModemChrome::Scale::Compact}) {
        const QString sheet = ModemChrome::styleSheet(scale);
        const int checkedAt = sheet.indexOf(QStringLiteral("QPushButton:checked {"));
        const int disabledAt = sheet.indexOf(QStringLiteral("QPushButton:disabled {"));
        QVERIFY2(checkedAt >= 0, "QPushButton:checked rule is gone");
        QVERIFY2(disabledAt >= 0, "QPushButton:disabled rule is gone");
        QVERIFY2(checkedAt < disabledAt,
                 "QPushButton:checked must precede :disabled, or a disabled "
                 "checked button paints as latched");
    }
}

// Not QTEST_MAIN: ThemeManager is a singleton that reads ActiveTheme from
// AppSettings on construction and writes it back (ThemeManager.cpp:367 and
// :669-670). Without an isolated profile the run would mutate the real
// settings database of whoever executes it, and -- the part that breaks the
// assertions -- everyThemeTokenInTheSheetResolves would resolve tokens
// against whatever theme that store happens to name. A user theme missing
// color.meter.gainReduction would fail the test for reasons unrelated to the
// sheet, and one defining a token the shipped themes lack would mask exactly
// the class of bug the test exists to catch. The profile therefore has to
// exist before the first instance() call, which means before qExec.
int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-modem-chrome-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);
    // Pin the baseline explicitly rather than relying on the empty profile
    // falling back to it, so the tokens are checked against a theme this test
    // names.
    ThemeManager::instance().setActiveTheme(QStringLiteral("Default Dark"));
    ModemChromeTest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "modem_chrome_test.moc"
