#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/AppSettings.h"
#include "gui/ConnectionPanel.h"
#include "models/RadioModel.h"
#include "models/NetworkRadioLookup.h"
#include "models/NetworkRadioLogin.h"
#include "core/backends/icom/IcomCredentials.h"
#include <QEvent>
#include <QJsonObject>
#include <QJsonDocument>

#include <QApplication>
#include <QHostInfo>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <cstdlib>
#include <iostream>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject disconnect(AutomationServer& server) { return server.doDisconnect(); }
    static QJsonObject connect(AutomationServer& server, const QString& argument) {
        return server.doConnect(QStringLiteral("ip"), argument, nullptr);
    }
    static QJsonObject local(AutomationServer& server, const QString& serial) {
        return server.doConnect(QStringLiteral("local"), QStringLiteral("serial ") + serial, nullptr);
    }
};
struct RadioModelWakeTestAccess {
    static void arm(RadioModel& model) {
        model.m_intentionalDisconnect = false;
        model.m_connectAttemptActive = true;
        model.m_reconnectTimer.start(60000);
    }
    static bool cancelled(const RadioModel& model) {
        return model.m_intentionalDisconnect && !model.m_connectAttemptActive
            && !model.m_reconnectTimer.isActive();
    }
};
struct ConnectionPanelStartupTestAccess {
    static void prepare(ConnectionPanel& panel,
                        std::function<void(const QHostInfo&)>& lookup,
                        std::function<void(const QString&)>& password) {
        panel.m_networkLoginLoader = [](const QString&, QObject*, std::function<void(NetworkRadioLogin)> done) { done({}); };
        panel.m_icomPasswordLoader = [&password](QObject*, std::function<void(const QString&)> done) { password = std::move(done); };
        panel.m_networkHostLookup = new NetworkRadioLookup(
            [&lookup](const QString&, QObject*, std::function<void(const QHostInfo&)> done) {
                lookup = std::move(done); return 17;
            }, [](int) {}, 60000, &panel);
    }
    static void pendingLookup(ConnectionPanel& panel) {
        panel.m_manualIpEdit->setText(QStringLiteral("cancel-test.invalid"));
        panel.m_manualNetworkUser->setText(QStringLiteral("fixture-user"));
        panel.m_manualNetworkPassword->setText(QStringLiteral("fixture-password"));
        panel.onManualConnectClicked();
    }
    static void choose(ConnectionPanel& panel, const QString& family) { panel.setManualFamily(family); }
    static void pendingIcom(ConnectionPanel& panel) {
        panel.m_manualIpEdit->setText(QStringLiteral("192.0.2.99"));
        panel.m_manualIcomUserEdit->setText(QStringLiteral("fixture-user"));
        panel.m_manualIcomPassEdit->clear();
        panel.onManualConnectClicked();
    }
    static void ownSocket(ConnectionPanel& panel, QTcpSocket* socket) { panel.m_manualFlexProbe = socket; }
    static void earlierSocket(ConnectionPanel& panel, QTcpSocket* socket) { panel.m_manualFlexProbes.append(socket); }
    static void cancel(ConnectionPanel& panel) { panel.cancelNetworkAttempt(); }
    static bool pending(const ConnectionPanel& panel) { return panel.m_manualConnectPending; }
    static void startup(ConnectionPanel& panel, const QString& host) { panel.probeRadio(host, true); }
    static void discover(ConnectionPanel& panel, const RadioInfo& radio) { panel.onRadioDiscovered(radio); }
    static void wan(ConnectionPanel& panel) {
        panel.m_wanRadios = {WanRadioInfo{}};
        panel.m_wanList->addItem(QStringLiteral("fixture"));
        panel.m_wanList->setCurrentRow(0);
        panel.onWanConnectClicked();
    }
};
}
using namespace AetherSDR;
namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
}
int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("automation-disconnect-pending"));
    QApplication app(argc, argv);
    require(profile.isValid(), "isolated settings");
    AppSettings::instance().load();
    std::function<void(const QHostInfo&)> lookup;
    std::function<void(const QString&)> password;
    ConnectionPanel panel;
    ConnectionPanelStartupTestAccess::prepare(panel, lookup, password);
    RadioModel model;
    AutomationServer server;
    server.setRadioModel(&model);
    server.setConnectionAutomation(&panel);
    QObject::connect(&panel, &ConnectionPanel::disconnectRequested, &model, &RadioModel::disconnectFromRadio);
    QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
    QSignalSpy disconnects(&panel, &ConnectionPanel::disconnectRequested);
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("kenwood"));
    ConnectionPanelStartupTestAccess::pendingLookup(panel);
    require(static_cast<bool>(lookup), "injected lookup is pending");
    RadioModelWakeTestAccess::arm(model);
    server.setRadioModel(&model);
    AutomationServerTestAccess::connect(server, QStringLiteral("192.0.2.99 icom"));
    require(AutomationServerTestAccess::disconnect(server).value("ok").toBool(), "pending disconnect accepted");
    require(disconnects.isEmpty(), "production callback is deferred");
    settle();
    require(disconnects.size() == 1 && RadioModelWakeTestAccess::cancelled(model), "actual panel signal stops reconnect timer between attempts");
    QHostInfo answer;
    answer.setAddresses({QHostAddress(QStringLiteral("192.0.2.99"))});
    lookup(answer);
    model.triggerAutoReconnectForTest();
    settle();
    require(connects.isEmpty() && !ConnectionPanelStartupTestAccess::pending(panel)
        && RadioModelWakeTestAccess::cancelled(model), "retired DNS and retry cannot resurrect connection");
    // Unconnected sockets exercise ownership without creating a network peer.
    QPointer<QTcpSocket> owned = new QTcpSocket(&panel);
    QPointer<QTcpSocket> unrelated = new QTcpSocket(&panel);
    QPointer<QTcpSocket> earlier = new QTcpSocket(&panel);
    ConnectionPanelStartupTestAccess::earlierSocket(panel, earlier);
    ConnectionPanelStartupTestAccess::ownSocket(panel, owned);
    panel.automationDisconnect();
    settle();
    require(!owned && !earlier && unrelated, "native cancellation retires every panel-owned probe, including an older quiet probe, while preserving unrelated sockets");
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("flex"));
    QPointer<QTcpSocket> legacyProbe = new QTcpSocket(&panel);
    ConnectionPanelStartupTestAccess::ownSocket(panel, legacyProbe);
    const quint64 legacyEpoch = panel.automationNativeConnectionEpoch();
    require(!panel.automationUsesDeferredNetworkLogin()
        && panel.automationUsesDeferredNetworkLogin(QStringLiteral("kenwood")),
        "only an actual or requested native login selects deferred semantics");
    require(!panel.automationDisconnect(), "ordinary idle Flex disconnect retains baseline refusal");
    ConnectionPanelStartupTestAccess::cancel(panel);
    settle();
    require(legacyProbe && panel.automationNativeConnectionEpoch() == legacyEpoch,
        "ordinary established-radio actions neither cancel Flex probes nor advance native epochs");
    IcomCredentials::clearSession();
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("icom"));
    ConnectionPanelStartupTestAccess::pendingIcom(panel);
    require(static_cast<bool>(password), "Icom credential completion is injected without vault access");
    require(!panel.automationDisconnect(), "idle Icom disconnect retains its baseline refusal");
    password(QStringLiteral("fixture-password"));
    settle();
    require(connects.size() == 1, "ordinary Icom credential completion retains baseline dispatch");
    connects.clear();
    IcomCredentials::clearSession();
    ConnectionPanelStartupTestAccess::pendingIcom(panel);
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("kenwood"));
    panel.automationDisconnect();
    password(QStringLiteral("fixture-password"));
    settle();
    require(connects.isEmpty(), "entering deferred native login retires an older Icom credential completion");
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("icom"));
    panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))->setText(QStringLiteral("fixture-password"));
    const int beforeQueuedConnect = connects.size();
    const int beforeQueuedDisconnect = disconnects.size();
    require(!AutomationServerTestAccess::disconnect(server).value("ok").toBool(),
            "ordinary idle Icom bridge disconnect is not accepted as native cancellation");
    AutomationServerTestAccess::connect(server, QStringLiteral("192.0.2.99 icom"));
    settle();
    require(connects.size() == beforeQueuedConnect + 1 && disconnects.size() == beforeQueuedDisconnect,
            "newer queued bridge Connect survives earlier queued Disconnect");
    panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))->clear();
    IcomCredentials::clearSession();
    const int before = disconnects.size();
    AutomationServerTestAccess::disconnect(server);
    ConnectionPanelStartupTestAccess::pendingIcom(panel);
    settle();
    require(disconnects.size() == before, "old disconnect does not cancel newer explicit Connect on same panel/model");
    panel.automationDisconnect();
    panel.setConnected(true);
    const int connectedBefore = disconnects.size();
    require(panel.automationDisconnect(), "connected established-radio panel accepts disconnect");
    settle();
    require(disconnects.size() == connectedBefore + 1, "connected path still emits the production disconnect intent");
    panel.setConnected(false);
    for (bool wan : {false, true}) {
        AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
        ConnectionPanel legacy;
        ConnectionPanelStartupTestAccess::prepare(legacy, lookup, password);
        ConnectionPanelStartupTestAccess::choose(legacy, QStringLiteral("kenwood"));
        ConnectionPanelStartupTestAccess::pendingLookup(legacy);
        const std::function<void(const QHostInfo&)> retiredLookup = lookup;
        const quint64 beforeNativeRetirement = legacy.automationNativeConnectionEpoch();
        RadioInfo local;
        local.family = QStringLiteral("flex");
        local.serial = QStringLiteral("legacy-fixture");
        local.model = QStringLiteral("FLEX-8600");
        ConnectionPanelStartupTestAccess::discover(legacy, local);
        QSignalSpy legacyConnects(&legacy, &ConnectionPanel::connectRequested);
        QSignalSpy wanConnects(&legacy, &ConnectionPanel::wanConnectRequested);
        if (wan) { ConnectionPanelStartupTestAccess::wan(legacy); }
        else { require(legacy.automationConnectLocalSerial(local.serial), "local connect accepted"); }
        require(legacy.automationNativeConnectionEpoch() != beforeNativeRetirement,
            "Local/WAN takeover retires old native callbacks");
        require(!legacy.automationUsesDeferredNetworkLogin(),
            "Local/WAN takeover retires native ordering despite the retained Kenwood form");
        require(legacy.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"))->currentData()
            == QStringLiteral("kenwood"), "takeover preserves the saved manual family");
        retiredLookup(answer);
        settle();
        require(legacyConnects.size() == (wan ? 0 : 1) && wanConnects.size() == (wan ? 1 : 0),
            "retired native DNS cannot connect after Local/WAN takeover");
        legacy.setConnected(true);
        legacy.setConnected(false);
        server.setConnectionAutomation(&legacy);
        require(!legacy.automationDisconnect()
            && !AutomationServerTestAccess::disconnect(server).value("ok").toBool(),
            "idle legacy disconnect retains its baseline refusal after native form use");
        legacyConnects.clear();
        require(AutomationServerTestAccess::local(server, local.serial).value("ok").toBool()
            && AutomationServerTestAccess::local(server, local.serial).value("ok").toBool(),
            "two ordinary queued local requests are accepted");
        settle();
        require(legacyConnects.size() == 2, "ordinary queued local requests keep legacy ordering");
        legacyConnects.clear();
        const QJsonObject nativeConnect = AutomationServerTestAccess::connect(server, QStringLiteral("queued-native.invalid"));
        require(nativeConnect.value("ok").toBool()
            && AutomationServerTestAccess::disconnect(server).value("ok").toBool(),
            "a queued dialog-selected native connection admits a newer cancellation after legacy takeover");
        settle();
        require(legacyConnects.isEmpty() && !ConnectionPanelStartupTestAccess::pending(legacy),
            "a newer disconnect retires the queued native connect before its login starts");
        server.setConnectionAutomation(&panel);
    }
    for (bool targetNative : {false, true}) {
        IcomCredentials::clearSession();
        AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
        AppSettings::instance().setValue("RoutedProfilesJson", "{}");
        ConnectionPanel restored;
        ConnectionPanelStartupTestAccess::prepare(restored, lookup, password);
        ConnectionPanelStartupTestAccess::choose(restored,
            targetNative ? QStringLiteral("flex") : QStringLiteral("kenwood"));
        const QString host = targetNative ? QStringLiteral("restored-radio.invalid") : QStringLiteral("192.0.2.123");
        const QString family = targetNative ? QStringLiteral("kenwood") : QStringLiteral("icom");
        const QJsonObject route{{QStringLiteral("identity"), QJsonObject{{QStringLiteral("family"), family}}},
            {QStringLiteral("bind"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("auto")}}}};
        AppSettings::instance().setValue("RoutedProfilesJson", QString::fromUtf8(
            QJsonDocument(QJsonObject{{host, route}}).toJson(QJsonDocument::Compact)));
        restored.findChild<QLineEdit*>(QStringLiteral("connectionNetworkUsername"))->setText(QStringLiteral("fixture-user"));
        restored.findChild<QLineEdit*>(QStringLiteral("connectionNetworkPassword"))->setText(QStringLiteral("fixture-password"));
        restored.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomUser"))->setText(QStringLiteral("fixture-user"));
        restored.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))->clear();
        QSignalSpy restoredConnects(&restored, &ConnectionPanel::connectRequested);
        ConnectionPanelStartupTestAccess::startup(restored, host);
        require(restored.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"))->currentData() == family,
            "startup restores the routed family before probing");
        if (targetNative) {
            require(static_cast<bool>(lookup), "restored native family starts the injected lookup");
            lookup(answer);
        } else {
            require(static_cast<bool>(password), "restored Icom family waits for the injected password");
            password(QStringLiteral("fixture-password"));
        }
        settle();
        require(restoredConnects.size() == 1,
            "the current startup completion survives its own saved-family restoration");
        require(qvariant_cast<RadioInfo>(restoredConnects.first().first()).family == family,
            "restored startup dispatches exactly the selected family");
    }
    return 0;
}
