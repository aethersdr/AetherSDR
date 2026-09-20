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

#include <QRegularExpression>
#include <QString>
#include <QtTest>

using namespace AetherSDR;

class ModemChromeTest : public QObject {
    Q_OBJECT

private slots:
    void bothScalesCarryEveryStructuralSelector();
    void tabsAreSelectedByPropertyNotObjectName();
    void everyPlaceholderIsSubstituted();
    void compactIsSmallerButNotADifferentPalette();
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

QTEST_MAIN(ModemChromeTest)
#include "modem_chrome_test.moc"
