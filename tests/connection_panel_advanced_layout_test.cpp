// Connect-to-Radio geometry guard for the manual/VPN page.
//
// Regression cover for the squashed "Advanced: choose the VPN source path"
// section: with the dialog shorter than its layout needed, Qt handed rows less
// than their own minimum heights. The source-path QComboBox rendered as a
// sliver and the word-wrapped hint above it was clipped to a sliced single line
// that overlapped the row below.
//
// What holds the dialog tall enough here is the *constructor-time*
// ConnectionPanel::refitToContent() (ConnectionPanel.cpp:871), not a refit
// triggered by revealing the section. Expanding via the toggle runs
// onManualAdvancedToggled(), which only flips the arrow and the widget's
// visibility; the refit on the reveal path lives in
// updateManualAdvancedVisibility(), which fires when a saved source path goes
// stale, not when the operator clicks. So the floor under assertion is the one
// the constructor derives from the built layout.
//
// Both assertions are load-bearing — stubbing refitToContent() to an early
// return drops the source-path combo to 10px (it asks for 25) and the hint to
// 10px (its wrapped text needs 30), reproducing the report.
//
// Deliberately not asserted here, both measured before being dropped:
//   * The rows above the section. They keep their minimums even when the dialog
//     cannot grow, because the squashing is absorbed by the section's
//     word-wrapped labels first — so such a check passes in both states.
//   * minimumHeight() >= layout()->minimumSize().height(), i.e. the floor as a
//     mechanism rather than its consequence. It looks like the sharper
//     assertion and is worthless: with refitToContent() stubbed the panel has
//     no explicit minimum, so QLayout's SetDefaultConstraint sets one from the
//     layout itself and the comparison is true by construction. It passes in
//     both states. What actually discriminates is the height each widget is
//     *allocated*, which is what the two tests below measure.
//
// Build: CMake target `connection_panel_advanced_layout_test`.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/ConnectionPanel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QTest>
#include <QToolButton>
#include <QWidget>

using AetherSDR::AppSettings;
using AetherSDR::ConnectionPanel;

namespace {

void settle(ConnectionPanel& panel)
{
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    if (auto* panelLayout = panel.layout())
        panelLayout->activate();
    QCoreApplication::processEvents();
}

// Show the panel on its manual/VPN page, the way a user hitting this bug does.
// Reports failure to the caller rather than using QVERIFY: a bare return from a
// void helper leaves the calling slot running on an unprepared panel, which
// turns one missing widget into a page of cascading failures.
[[nodiscard]] bool showOnManualPage(ConnectionPanel& panel)
{
    panel.show();
    if (!QTest::qWaitForWindowExposed(&panel))
        return false;
    auto* manualMode = panel.findChild<QAbstractButton*>("connectionManualModeButton");
    if (!manualMode)
        return false;
    manualMode->click();
    settle(panel);
    return true;
}

// The advanced section only auto-reveals when the host has more than one IPv4
// candidate, which a test machine cannot be relied on to have — drive the
// widgets directly so the geometry under test is deterministic.
[[nodiscard]] bool expandAdvancedSection(ConnectionPanel& panel)
{
    auto* toggle = panel.findChild<QToolButton*>("connectionManualAdvancedToggle");
    auto* section = panel.findChild<QWidget*>("connectionManualAdvancedSection");
    if (!toggle || !section)
        return false;
    toggle->setVisible(true);
    toggle->setChecked(true);
    section->setVisible(true);
    settle(panel);
    return true;
}

}  // namespace

class ConnectionPanelAdvancedLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    // The reported symptom: the "Source path:" combo squashed to a sliver.
    void sourcePathComboKeepsItsMinimumHeight()
    {
        ConnectionPanel panel;
        QVERIFY2(showOnManualPage(panel), "could not open the manual/VPN page");
        QVERIFY2(expandAdvancedSection(panel), "could not expand the advanced section");

        auto* combo = panel.findChild<QComboBox*>("connectionManualSourcePath");
        QVERIFY(combo);
        QVERIFY(combo->isVisible());
        const int wanted = combo->minimumSizeHint().height();
        QVERIFY2(combo->height() >= wanted,
                 qPrintable(QStringLiteral("source path combo is %1px tall, needs %2px")
                                .arg(combo->height())
                                .arg(wanted)));
    }

    // The other half of the screenshot: the wrapped hint clipped to a single
    // sliced line, bleeding into the row below.
    void advancedHintIsNotClipped()
    {
        ConnectionPanel panel;
        QVERIFY2(showOnManualPage(panel), "could not open the manual/VPN page");
        QVERIFY2(expandAdvancedSection(panel), "could not expand the advanced section");

        auto* hint = panel.findChild<QLabel*>("connectionManualAdvancedHint");
        QVERIFY(hint);
        QVERIFY(hint->isVisible());
        const int wrapped = hint->heightForWidth(hint->width());
        QVERIFY2(hint->height() >= wrapped,
                 qPrintable(QStringLiteral("advanced hint is %1px tall, the wrapped text needs "
                                           "%2px at width %3")
                                .arg(hint->height())
                                .arg(wrapped)
                                .arg(hint->width())));
    }
};

// Not QTEST_MAIN: the settings sandbox has to exist before QApplication and
// before the first AppSettings access, which that macro makes impossible. The
// panel reads the operator's live store (FramelessWindow,
// ConnectByIpRadioFamily, theme) and FramelessWindow in particular changes the
// title bar and the root margins — i.e. the exact geometry under assertion.
int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-connection-panel-advanced-layout-test"));
    if (!settingsProfile.isValid())
        return 1;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    AppSettings::instance().load();
    // Chrome is part of what is measured, so pin it to the product default
    // rather than inheriting whatever the operator's store happens to hold.
    AppSettings::instance().setValue("FramelessWindow", "True");

    ConnectionPanelAdvancedLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "connection_panel_advanced_layout_test.moc"
