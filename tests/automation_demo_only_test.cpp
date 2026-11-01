// No socket can be opened by this harness, even when admission is mutated:
// the model uses an inert backend; RadioConnection is never init()ialized;
// the native picker has no connection receiver; manual probing uses an empty target.
#include "TestSettingsProfile.h"
#include "core/AutomationIsolationPolicy.h"
#include "core/AutomationServer.h"
#include "core/KiwiSdrClient.h"
#include "core/TcpByteProxy.h"
#include "core/Ctr2UsbRelay.h"
#include "core/backends/sim/DemoRadioConstants.h"
#include "core/backends/flex/RadioConnection.h"
#include "gui/ConnectionPanel.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QHostAddress>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStringList>
#include <cstdio>
#include <memory>

namespace AetherSDR {
struct RadioModelWakeTestAccess {
    static void reconnectTarget(RadioModel& model, const RadioInfo& info)
    {
        model.m_lastInfo = info;
        model.m_intentionalDisconnect = false;
    }
};
struct ConnectionPanelStartupTestAccess {
    static void clickLocal(ConnectionPanel& panel) { panel.m_localConnectBtn->click(); }
    static QString status(const ConnectionPanel& panel) { return panel.m_statusLabel->text(); }
    static QString manualResult(const ConnectionPanel& panel) { return panel.m_manualResultLabel->text(); }
    static void clickWan(ConnectionPanel& panel)
    {
        WanRadioInfo info;
        info.serial = QStringLiteral("synthetic-test-target");
        panel.m_wanRadios.append(info);
        panel.m_wanList->addItem(QStringLiteral("Synthetic target"));
        panel.m_wanList->setCurrentRow(0);
        panel.onWanConnectClicked();
        panel.onWanDisconnectClientsClicked();
    }
};
class AutomationServerTestAccess {
public:
    static QJsonObject whoami(AutomationServer& server) { return server.doWhoami(); }
};
}
using namespace AetherSDR;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* name)
{
    ++checks;
    failures += !ok;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
class InertBackend final : public IRadioBackend {
public:
    int attempts = 0;
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return false; }
    void connectRadio(const RadioConnectRequest&) override { ++attempts; }
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
}
int main(int argc, char** argv)
{
    const bool restricted = argc > 1 && QByteArray(argv[1]) == "restricted";
    qunsetenv("AETHER_AUTOMATION_DEMO_ONLY");
    qputenv("AETHER_AUTOMATION", "1");
    if (restricted) { qputenv("AETHER_AUTOMATION_DEMO_ONLY", "0"); }
    check(AutomationIsolationPolicy::demoOnly() == restricted,
          "presence including zero enables restriction; automation alone does not");
    if (restricted) { qunsetenv("AETHER_AUTOMATION_DEMO_ONLY"); }
    else { qputenv("AETHER_AUTOMATION_DEMO_ONLY", "1"); }
    check(AutomationIsolationPolicy::demoOnly() == restricted,
          "policy remains pinned after environment changes");
    check(AutomationIsolationPolicy::allowsRadio(DemoRadio::family(), DemoRadio::serial()),
          "canonical simulator identity is admitted");
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("hl2"),
         QStringLiteral("anan"), QStringLiteral("icom"), QStringLiteral("rtl"), QString()}) {
        check(AutomationIsolationPolicy::allowsRadio(family, DemoRadio::serial()) == !restricted,
              "non-simulator family cannot borrow the demo serial");
    }
    check(AutomationIsolationPolicy::allowsRadio(DemoRadio::family(), QStringLiteral("other")) == !restricted,
          "simulator family alone cannot authorize another identity");
    TestSettingsProfile profile(QStringLiteral("automation-demo-only"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AutomationServer bridge;
    check(AutomationServerTestAccess::whoami(bridge).value(QStringLiteral("demoOnly")).toBool() == restricted,
          "whoami reports the pinned process policy without starting a socket");
    if (!restricted) {
        ConnectionPanel ordinary;
        QSignalSpy requested(&ordinary, &ConnectionPanel::connectRequested);
        RadioInfo physical;
        physical.family = QStringLiteral("flex");
        physical.serial = QStringLiteral("synthetic-normal-target");
        ordinary.onRadioDiscovered(physical);
        ConnectionPanelStartupTestAccess::clickLocal(ordinary);
        check(requested.size() == 1, "ordinary native Connect retains its existing signal behavior");
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    RadioModel model;
    auto backend = std::make_unique<InertBackend>();
    InertBackend* inert = backend.get();
    model.setBackendForTest(std::move(backend), DemoRadio::family());
    RadioInfo target;
    target.family = DemoRadio::family();
    target.serial = QStringLiteral("synthetic-test-target");
    target.address = QHostAddress(QHostAddress::LocalHost);
    QSignalSpy errors(&model, &RadioModel::connectionError);
    model.connectToRadio(target);
    check(errors.size() == 1 && inert->attempts == 0,
          "model rejects mismatched identity before dispatch to injected backend");
    check(!model.isConnectAttemptInFlight() && model.serial().isEmpty(),
          "rejected model admission leaves connection identity and attempt state unchanged");
    RadioModelWakeTestAccess::reconnectTarget(model, target);
    model.triggerAutoReconnectForTest();
    check(errors.size() == 2 && inert->attempts == 0 && !model.isConnectAttemptInFlight(),
          "automatic reconnect rejects before changing state or dispatching");
    QString error;
    check(!model.wakeIcomRadio(1, 1, &error) && error == AutomationIsolationPolicy::refusal(),
          "wake refuses before backend or reconnect work");
    // Unknown family has no telemetry provider even if this guard regresses.
    check(model.setOfflineHealthTarget(QStringLiteral("synthetic-no-provider"), target.address)
              == RadioModel::OfflineAimResult::DemoOnly && !model.hasOfflineHealth(),
          "telemetry refuses before provider construction");
    check(model.setOfflineHealthTarget(QStringLiteral("synthetic-no-provider"), QHostAddress())
              == RadioModel::OfflineAimResult::Ok,
          "telemetry target-off remains harmless and available");
    target.serial = DemoRadio::serial();
    model.connectToRadio(target);
    check(inert->attempts == 1 && model.isConnectAttemptInFlight(),
          "canonical simulator reaches the model's injected backend");

    // init() is intentionally absent: no QTcpSocket exists even without the guard.
    RadioConnection transport;
    QSignalSpy transportErrors(&transport, &RadioConnection::errorOccurred);
    transport.connectToHost(QHostAddress(QHostAddress::LocalHost), 4992);
    check(transportErrors.size() == 1 && transport.radioAddress().isNull(),
          "direct TCP entry refuses before initialization or address mutation");
    target.family = QStringLiteral("flex");
    transport.connectToRadio(target);
    check(transportErrors.size() == 2 && !transport.isSyntheticDemo(),
          "transport rejects a real family using the demo serial");
    target.family = DemoRadio::family();
    transport.connectToRadio(target);
    check(transport.isSyntheticDemo(), "canonical simulator uses the socket-free synthetic transport");
    transport.disconnectFromRadio();

    TcpByteProxy proxy;
    check(!proxy.start({}) && proxy.lastError() == AutomationIsolationPolicy::refusal(),
          "direct TCP relay refuses before validating an empty configuration or binding");
    Ctr2UsbRelay usb;
    check(!usb.start(nullptr, QHostAddress(), 0) && usb.lastError() == AutomationIsolationPolicy::refusal(),
          "USB radio relay refuses before accessing a device or starting a session");

    KiwiSdrClient kiwi;
    QSignalSpy kiwiState(&kiwi, &KiwiSdrClient::stateChanged);
    kiwi.connectToEndpoint(QString(), QString()); // invalid even if admission is removed
    check(kiwi.state() == KiwiSdrClient::State::Error && kiwi.endpoint().isEmpty()
              && kiwiState.size() == 1
              && kiwiState.at(0).at(1).toString() == AutomationIsolationPolicy::refusal(),
          "Kiwi admission refuses before endpoint parsing or status HTTP");

    ConnectionPanel panel;
    QSignalSpy local(&panel, &ConnectionPanel::connectRequested);
    QSignalSpy wan(&panel, &ConnectionPanel::wanConnectRequested);
    QSignalSpy disconnects(&panel, &ConnectionPanel::wanDisconnectClientsRequested);
    RadioInfo physical;
    physical.family = QStringLiteral("flex");
    physical.serial = QStringLiteral("synthetic-physical-target");
    physical.name = QStringLiteral("Synthetic target");
    panel.onRadioDiscovered(physical);
    ConnectionPanelStartupTestAccess::clickLocal(panel);
    check(local.isEmpty() && ConnectionPanelStartupTestAccess::status(panel) == AutomationIsolationPolicy::refusal(),
          "native Connect button refuses a physical target before emitting intent");
    error.clear();
    check(!panel.automationConnectLocalSerial(physical.serial, &error)
              && error == AutomationIsolationPolicy::refusal() && local.isEmpty(),
          "automation local selection refuses the same physical target");
    panel.probeRadio(QString());
    check(ConnectionPanelStartupTestAccess::manualResult(panel).contains(AutomationIsolationPolicy::refusal()),
          "manual probe entry refuses before even parsing an empty target");
    check(!panel.automationConnectByIp(QString(), QStringLiteral("flex"), &error)
              && error == AutomationIsolationPolicy::refusal(),
          "automation manual target refuses before editing the connection form");
    ConnectionPanelStartupTestAccess::clickWan(panel);
    check(wan.isEmpty() && disconnects.isEmpty(), "native WAN connect and remote-client actions are refused");
    panel.addDemoRadio();
    check(panel.automationConnectLocalSerial(DemoRadio::serial(), &error) && local.size() == 1,
          "native demo selection still emits its canonical connection intent");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
