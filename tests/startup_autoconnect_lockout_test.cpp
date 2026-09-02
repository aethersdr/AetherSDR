// Regression harness: a startup auto-connect that gives up must hand the window
// back to the operator.
//
// MainWindow suppresses its "no saved radio" connection-dialog popup whenever
// LastConnectedRadioSerial is set, and covers the window with a
// "Looking for your radio…" overlay instead. So when the saved radio cannot be
// reached, a bail that only calls setManualMessage() writes the reason onto a
// page nobody can see, behind a dialog that never opens — leaving a spinner and
// no offered route back into the connection UI.
//
// ConnectionPanel::startupConnectUnavailable is what closes that hole. These
// checks pin the contract MainWindow relies on: startup bails report upward,
// interactive ones stay silent (their operator is already reading the panel).

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/icom/IcomSettings.h"
#include "gui/ConnectionPanel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// Puts the panel on the manual page with the given family selected.
void selectManualFamily(ConnectionPanel& panel, const char* family)
{
    if (auto* manualMode =
            panel.findChild<QAbstractButton*>(QStringLiteral("connectionManualModeButton"))) {
        manualMode->click();
    }
    auto* radioType =
        panel.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"));
    const int index = radioType ? radioType->findData(QString::fromLatin1(family)) : -1;
    if (radioType && index >= 0) {
        radioType->setCurrentIndex(index);
    }
    QApplication::processEvents();
}

// Icom with no credentials staged — the state a first launch after a
// credential loss is in, and a bail that needs no network at all.
void selectIcomManualFamily(ConnectionPanel& panel)
{
    selectManualFamily(panel, ConnectionPanel::kFamilyIcom);
    if (auto* user =
            panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomUser"))) {
        user->clear();
    }
    if (auto* pass =
            panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))) {
        pass->clear();
    }
    QApplication::processEvents();
}

// A startup probe with no credentials must not fail silently: it is the only
// probe whose failure the operator cannot read off the manual page.
void checkStartupBailIsReportedUpward()
{
    ConnectionPanel panel;
    selectIcomManualFamily(panel);

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);
    QApplication::processEvents();

    report("startup probe with no credentials reports upward",
           spy.count() == 1,
           "emitted " + std::to_string(spy.count()) + " time(s)");

    const QString reason = spy.isEmpty() ? QString()
                                         : spy.at(0).at(0).toString();
    report("reported reason is non-empty for the operator",
           !reason.trimmed().isEmpty(),
           reason.toStdString());
}

// The HL2 arm bails after a directed Metis probe rather than before it, so it
// is the path a wiring gap would most plausibly hide in: a saved Hermes-Lite 2
// that is powered off at boot must hand the window back like any other bail.
//
// This row sends one discovery datagram from an ephemeral UDP port to
// TEST-NET-1 (RFC 5737, never routable) and waits out the probe's own 600 ms
// deadline. There is no peer. Whether the datagram is dropped, refused by the
// local network stack, or the socket cannot bind at all, every one of those
// outcomes is a startup bail and must report upward — so the assertion holds
// regardless of the runner's network.
void checkHl2NoAnswerStartupBailIsReportedUpward()
{
    ConnectionPanel panel;
    selectManualFamily(panel, ConnectionPanel::kFamilyHl2);

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);
    QApplication::processEvents();

    report("startup HL2 probe with no answer reports upward",
           spy.count() == 1,
           "emitted " + std::to_string(spy.count()) + " time(s)");

    const QString reason = spy.isEmpty() ? QString()
                                         : spy.at(0).at(0).toString();
    report("HL2 reason names the address that did not answer",
           reason.contains(QStringLiteral("192.0.2.10")),
           reason.toStdString());
}

// The interactive path already shows its reason in the dialog the operator is
// looking at. Reporting upward there would pop the dialog they already have
// open — and MainWindow would overwrite the specific message with a status line.
void checkInteractiveBailStaysSilent()
{
    ConnectionPanel panel;
    selectIcomManualFamily(panel);

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/false);
    QApplication::processEvents();

    report("interactive probe stays silent",
           spy.count() == 0,
           "emitted " + std::to_string(spy.count()) + " time(s)");
}

// The latch must not leak across attempts: one startup probe is one report,
// and a later interactive probe must not inherit the startup flag.
void checkLatchDoesNotLeakIntoInteractiveProbe()
{
    ConnectionPanel panel;
    selectIcomManualFamily(panel);

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);
    QApplication::processEvents();
    const int afterStartup = spy.count();

    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/false);
    QApplication::processEvents();

    report("startup latch clears after it reports",
           afterStartup == 1 && spy.count() == afterStartup,
           "startup=" + std::to_string(afterStartup)
               + " total=" + std::to_string(spy.count()));
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-startup-autoconnect-lockout-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    // A saved username would satisfy the credential gate this harness drives.
    IcomSettings::setUsername(QString());
    std::printf("Startup auto-connect lockout harness\n\n");

    checkStartupBailIsReportedUpward();
    checkHl2NoAnswerStartupBailIsReportedUpward();
    checkInteractiveBailStaysSilent();
    checkLatchDoesNotLeakIntoInteractiveProbe();

    std::printf("\n%s\n", g_failed == 0 ? "All checks passed." : "FAILURES PRESENT.");
    return g_failed == 0 ? 0 : 1;
}
