#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "gui/ConnectionPanel.h"
#include "models/RadioModel.h"
#include "models/NetworkRadioLookup.h"
#include "models/NetworkRadioLogin.h"
#include "core/backends/icom/IcomCredentials.h"
#include <QEvent>
#include <QJsonObject>

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
    static bool pending(const ConnectionPanel& panel) { return panel.m_manualConnectPending; }
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
    ConnectionPanelStartupTestAccess::ownSocket(panel, owned);
    panel.automationDisconnect();
    settle();
    require(!owned && unrelated, "cancellation deletes only panel-owned Flex probe");
    IcomCredentials::clearSession();
    ConnectionPanelStartupTestAccess::choose(panel, QStringLiteral("icom"));
    ConnectionPanelStartupTestAccess::pendingIcom(panel);
    require(static_cast<bool>(password), "Icom credential completion is injected without vault access");
    panel.automationDisconnect();
    password(QStringLiteral("fixture-password"));
    settle();
    require(connects.isEmpty(), "retired Icom credential callback cannot connect");
    panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIcomPassword"))->setText(QStringLiteral("fixture-password"));
    const int beforeQueuedConnect = connects.size();
    const int beforeQueuedDisconnect = disconnects.size();
    AutomationServerTestAccess::disconnect(server);
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
    AutomationServerTestAccess::disconnect(server);
    settle();
    require(disconnects.size() == connectedBefore + 1, "connected path still emits the production disconnect intent");
    panel.setConnected(false);
    return 0;
}
