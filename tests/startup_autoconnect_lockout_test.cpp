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
#include "core/backends/icom/IcomCredentials.h"
#include "gui/ConnectionPanel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSignalBlocker>

#include <cstdio>
#include <string>

namespace AetherSDR {

// Inject only completed probe outcomes/state. No discovery object, transport,
// keychain job, MainWindow, or radio backend is created by this harness.
struct ConnectionPanelStartupTestAccess {
    static void arm(ConnectionPanel& panel) { panel.m_startupProbe = true; }
    static void noAnswer(ConnectionPanel& panel) {
        panel.handleHl2ProbeResult(ConnectionPanel::Hl2ProbeResult::NoAnswer,
                                   QStringLiteral("192.0.2.10"));
    }
    static void dispatch(ConnectionPanel& panel, bool routedOnly) {
        RadioInfo info;
        info.family = QStringLiteral("hl2");
        info.serial = QStringLiteral("injected-probe-result");
        panel.finishManualProbe(info, routedOnly);
    }
};

} // namespace AetherSDR

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
bool selectManualFamily(ConnectionPanel& panel, const char* family)
{
    if (auto* manualMode =
            panel.findChild<QAbstractButton*>(QStringLiteral("connectionManualModeButton"))) {
        manualMode->click();
    }
    auto* radioType =
        panel.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"));
    const int index = radioType ? radioType->findData(QString::fromLatin1(family)) : -1;
    if (radioType && index >= 0) {
        // Family hints normally start an OS keychain read for Icom. These rows
        // exercise the credential gate with explicitly staged fields instead.
        const QSignalBlocker blocker(radioType);
        radioType->setCurrentIndex(index);
        return true;
    }
    report("manual family selector exists", false);
    return false;
}

// Icom with no credentials staged — the state a first launch after a
// credential loss is in, and a bail that needs no network at all.
bool selectIcomManualFamily(ConnectionPanel& panel)
{
    if (!selectManualFamily(panel, ConnectionPanel::kFamilyIcom)) {
        return false;
    }
    if (auto* user =
            panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomUser"))) {
        user->clear();
    }
    if (auto* pass =
            panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))) {
        pass->clear();
    }
    return true;
}

// A startup probe with no credentials must not fail silently: it is the only
// probe whose failure the operator cannot read off the manual page.
void checkStartupBailIsReportedUpward()
{
    ConnectionPanel panel;
    if (!selectIcomManualFamily(panel)) {
        return; // Never fall through to a network family if the fixture changes.
    }

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);

    report("startup probe with no credentials reports upward",
           spy.count() == 1,
           "emitted " + std::to_string(spy.count()) + " time(s)");

    const QString reason = spy.isEmpty() ? QString()
                                         : spy.at(0).at(0).toString();
    report("reported reason is non-empty for the operator",
           !reason.trimmed().isEmpty(),
           reason.toStdString());
}

// Feed the production result handler its timeout result directly. A negative
// assertion is about our state transition, not whether a datagram goes missing.
void checkHl2NoAnswerStartupBailIsReportedUpward()
{
    ConnectionPanel panel;
    QSignalSpy failures(&panel, &ConnectionPanel::startupConnectUnavailable);
    QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
    ConnectionPanelStartupTestAccess::arm(panel);
    ConnectionPanelStartupTestAccess::noAnswer(panel);
    report("startup HL2 no-answer reports upward once", failures.count() == 1);
    const QString reason = failures.isEmpty() ? QString() : failures.at(0).at(0).toString();
    report("injected HL2 timeout identifies the attempted address",
           reason.contains(QStringLiteral("192.0.2.10")), reason.toStdString());
    report("no-answer never dispatches a connection", connects.isEmpty());
    ConnectionPanelStartupTestAccess::noAnswer(panel);
    report("HL2 failure clears startup ownership", failures.count() == 1);
}

void checkProbeDispatchEndsStartupOwnership()
{
    for (bool routedOnly : {false, true}) {
        ConnectionPanel panel;
        QSignalSpy failures(&panel, &ConnectionPanel::startupConnectUnavailable);
        QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
        QSignalSpy routed(&panel, &ConnectionPanel::routedRadioFound);
        ConnectionPanelStartupTestAccess::arm(panel);
        ConnectionPanelStartupTestAccess::dispatch(panel, routedOnly);
        report(routedOnly ? "probe dispatch emits routed discovery" : "probe dispatch emits connect",
               connects.count() == (routedOnly ? 0 : 1)
                   && routed.count() == (routedOnly ? 1 : 0));
        ConnectionPanelStartupTestAccess::noAnswer(panel);
        report(routedOnly ? "routed dispatch clears startup ownership" : "connect dispatch clears startup ownership",
               failures.isEmpty());
    }
}

void checkOperatorRouteEditsEndStartupOwnership()
{
    ConnectionPanel panel;
    QSignalSpy failures(&panel, &ConnectionPanel::startupConnectUnavailable);
    auto* host = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIp"));
    auto* family = panel.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"));
    report("operator route controls exist", host && family);
    if (!host || !family) {
        return;
    }
    ConnectionPanelStartupTestAccess::arm(panel);
    // textEdited/activated are the user-only signals; startup's setText and
    // saved-family restoration must not cancel the startup probe themselves.
    QMetaObject::invokeMethod(host, "textEdited", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("replacement-host")));
    ConnectionPanelStartupTestAccess::noAnswer(panel);
    report("operator host edit clears startup ownership", failures.isEmpty());
    ConnectionPanelStartupTestAccess::arm(panel);
    QMetaObject::invokeMethod(family, "activated", Qt::DirectConnection, Q_ARG(int, 0));
    ConnectionPanelStartupTestAccess::noAnswer(panel);
    report("operator family pick clears startup ownership", failures.isEmpty());
}

// The interactive path already shows its reason in the dialog the operator is
// looking at. Reporting upward there would pop the dialog they already have
// open — and MainWindow would overwrite the specific message with a status line.
void checkInteractiveBailStaysSilent()
{
    ConnectionPanel panel;
    if (!selectIcomManualFamily(panel)) {
        return; // Never fall through to a network family if the fixture changes.
    }

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/false);

    report("interactive probe stays silent",
           spy.count() == 0,
           "emitted " + std::to_string(spy.count()) + " time(s)");
}

// The latch must not leak across attempts: one startup probe is one report,
// and a later interactive probe must not inherit the startup flag.
void checkLatchDoesNotLeakIntoInteractiveProbe()
{
    ConnectionPanel panel;
    if (!selectIcomManualFamily(panel)) {
        return; // Never fall through to a network family if the fixture changes.
    }

    QSignalSpy spy(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);
    const int afterStartup = spy.count();

    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/false);

    report("startup latch clears after it reports",
           afterStartup == 1 && spy.count() == afterStartup,
           "startup=" + std::to_string(afterStartup)
               + " total=" + std::to_string(spy.count()));
}

// The real numeric-Icom probe stops at connectRequested: this standalone
// panel has no MainWindow/RadioModel recipient to create a radio transport.
// Explicit fields bypass the keychain and a numeric address bypasses DNS.
void checkIcomDispatchDoesNotLeakIntoLaterFailure()
{
    ConnectionPanel panel;
    if (!selectIcomManualFamily(panel)) {
        return;
    }
    auto* user = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomUser"));
    auto* pass = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"));
    report("Icom fixture fields exist", user && pass);
    if (!user || !pass) {
        return;
    }
    user->setText(QStringLiteral("fixture-user"));
    pass->setText(QStringLiteral("fixture-password"));
    QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
    QSignalSpy failures(&panel, &ConnectionPanel::startupConnectUnavailable);
    panel.probeRadio(QStringLiteral("192.0.2.10"), /*restoreSavedFamily=*/true);
    report("numeric Icom probe dispatches without a transport recipient", connects.count() == 1);
    panel.setConnected(false);
    user->clear();
    pass->clear();
    IcomSettings::setUsername(QString());
    IcomCredentials::clearSession();
    panel.probeRadio(QStringLiteral("192.0.2.10"));
    report("later Icom credential failure does not inherit startup", failures.isEmpty());
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
    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    std::printf("Startup auto-connect lockout harness\n\n");

    checkStartupBailIsReportedUpward();
    checkHl2NoAnswerStartupBailIsReportedUpward();
    checkProbeDispatchEndsStartupOwnership();
    checkOperatorRouteEditsEndStartupOwnership();
    checkInteractiveBailStaysSilent();
    checkLatchDoesNotLeakIntoInteractiveProbe();
    checkIcomDispatchDoesNotLeakIntoLaterFailure();

    std::printf("\n%s\n", g_failed == 0 ? "All checks passed." : "FAILURES PRESENT.");
    return g_failed == 0 ? 0 : 1;
}
