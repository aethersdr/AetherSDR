#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "gui/ConnectionPanel.h"
#include "models/NetworkRadioLogin.h"
#include "models/NetworkRadioLookup.h"

#include <QApplication>
#include <QHostInfo>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QPair>
#include <QTest>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR {
struct ConnectionPanelStartupTestAccess {
    static void loader(ConnectionPanel& panel, std::function<void(NetworkRadioLogin)>& pending)
    {
        pending = {};
        panel.m_networkLoginLoader = [&pending](const QString&, QObject*, std::function<void(NetworkRadioLogin)> callback) {
            pending = std::move(callback);
        };
    }
    static void lookup(ConnectionPanel& panel, std::function<void(const QHostInfo&)>& completion, int& cancelled)
    {
        panel.m_networkHostLookup = new NetworkRadioLookup(
            [&completion](const QString&, QObject*, std::function<void(const QHostInfo&)> callback) {
                completion = std::move(callback);
                return 123;
            }, [&cancelled](int) { ++cancelled; }, 10000, &panel);
    }
    static void connect(ConnectionPanel& panel) { panel.onManualConnectClicked(); }
    static void restore(ConnectionPanel& panel, const QString& host, bool restoreFamily = true)
    { panel.applySavedSourceSelection(host, restoreFamily); }
};
}
using namespace AetherSDR;
namespace {
void require(bool value, const char* claim)
{
    if (!value) { std::cerr << claim << '\n'; std::exit(1); }
}
template<class T> T* field(ConnectionPanel& panel, const char* name)
{
    T* result = panel.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, "connection field exists");
    return result;
}
void family(ConnectionPanel& panel, const char* value)
{
    QComboBox* combo = field<QComboBox>(panel, "connectionManualRadioType");
    const int index = combo->findData(QString::fromLatin1(value));
    require(index >= 0, "network profile is selectable");
    combo->setCurrentIndex(index);
}
void edited(QLineEdit* edit, const QString& value)
{
    edit->setText(value);
    QMetaObject::invokeMethod(edit, "textEdited", Qt::DirectConnection, Q_ARG(QString, value));
}
NetworkRadioLogin saved()
{
    NetworkRadioLogin login;
    login.host = QStringLiteral("192.0.2.5");
    login.username = QStringLiteral("stored-user");
    login.password = QStringLiteral("stored-secret");
    login.controlPort = 62000;
    login.administrator = true;
    login.variant = QStringLiteral("e");
    return login;
}
void verifyCredentialConsent(QObject& context)
{
    const NetworkRadioProfile profile = *networkRadioProfile(QStringLiteral("kenwood"));
    NetworkRadioLogin login = saved();
    login.host = QStringLiteral("192.0.2.80");
    const QString key = networkRadioCredentialKey(profile, login);
    QMap<QString, QString> vault{{key, login.password}};
    int reads = 0;
    int writes = 0;
    bool defer = false;
    std::function<void(QString)> pendingRead;
    NetworkRadioCredentialStore store;
    store.read = [&](const QString& entry, QObject*, std::function<void(QString)> callback) {
        ++reads;
        if (defer) { pendingRead = std::move(callback); }
        else { callback(vault.value(entry)); }
    };
    store.write = [&](const QString& entry, const QString& password, QObject*) {
        ++writes;
        vault.insert(entry, password);
    };
    AppSettings& settings = AppSettings::instance();
    const auto metadata = [&] {
        return QJsonDocument::fromJson(settings.value(profile.settingsKey).toString().toUtf8()).object();
    };
    const auto revision = [&] {
        RadioConnectRequest request;
        request.host = login.host;
        request.port = login.controlPort;
        populateNetworkRadioParams(request, profile.family);
        return request.params.value(QStringLiteral("connection.loginRevision")).toULongLong();
    };
    const QJsonObject legacy{{"SchemaVersion", 1}, {"Host", login.host}, {"Username", login.username},
        {"ControlPort", login.controlPort}, {"Administrator", login.administrator}};
    for (bool malformedConsent : {false, true}) {
        NetworkRadioLoginTestAccess::clearSession();
        QJsonObject document = legacy;
        if (malformedConsent) { document.insert("SavePassword", "true"); }
        settings.setValue(profile.settingsKey, QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact)));
        bool called = false;
        loadNetworkRadioLogin(profile.family, &context, [&](NetworkRadioLogin restored) {
            called = true;
            require(!restored.savePassword && restored.password.isEmpty(),
                "legacy or malformed opt-in restores no password and requires no vault permission");
        }, &store);
        require(called && reads == 0 && writes == 0 && vault.value(key) == login.password,
            "default session-only load never reads or deletes an existing keychain credential");
    }
    NetworkRadioLoginTestAccess::clearSession();
    stageNetworkRadioLogin(profile.family, login);
    commitNetworkRadioLogin(profile.family, login.host, revision(), &context, &store);
    require(writes == 0 && !metadata().value("SavePassword").toBool(true)
        && !metadata().contains("Password")
        && !QJsonDocument(metadata()).toJson().contains(login.password.toUtf8()),
        "successful session-only login saves only consent metadata, never plaintext or a vault password");
    loadNetworkRadioLogin(profile.family, &context, [&](NetworkRadioLogin restored) {
        require(restored.password == login.password && !restored.savePassword,
            "session-only password remains available for this authenticated endpoint");
    }, &store);
    require(reads == 0, "session cache reuse does not prompt the vault");
    login.savePassword = true;
    stageNetworkRadioLogin(profile.family, login);
    commitNetworkRadioLogin(profile.family, login.host, revision(), &context, &store);
    require(writes == 1 && metadata().value("SavePassword").toBool(false)
        && vault.value(key) == login.password, "explicit captured consent writes the endpoint-specific vault entry");
    NetworkRadioLoginTestAccess::clearSession();
    bool loaded = false;
    loadNetworkRadioLogin(profile.family, &context, [&](NetworkRadioLogin restored) {
        loaded = true;
        require(restored.savePassword && restored.password == login.password,
            "only explicit saved consent may reload an endpoint password from the vault");
    }, &store);
    require(loaded && reads == 1, "opt-in reload invokes the injected vault exactly once");
    stageNetworkRadioLogin(profile.family, login);
    const quint64 retired = revision();
    invalidateNetworkRadioLoginRevision(profile.family);
    loadNetworkRadioLogin(profile.family, &context, [&](NetworkRadioLogin restored) {
        require(!restored.savePassword,
            "retired authorization cannot reappear checked when the session cache is loaded again");
    }, &store);
    commitNetworkRadioLogin(profile.family, login.host, retired, &context, &store);
    require(writes == 1, "operator edits retire an earlier authenticated password-save authorization");
    stageNetworkRadioLogin(profile.family, login);
    const quint64 oldConsent = revision();
    login.savePassword = false;
    stageNetworkRadioLogin(profile.family, login);
    commitNetworkRadioLogin(profile.family, login.host, oldConsent, &context, &store);
    commitNetworkRadioLogin(profile.family, login.host, revision(), &context, &store);
    require(writes == 1 && !metadata().value("SavePassword").toBool(true)
        && vault.value(key) == login.password,
        "new session-only consent cannot inherit an older write revision or delete an existing vault entry");
    login.savePassword = true;
    stageNetworkRadioLogin(profile.family, login);
    commitNetworkRadioLogin(profile.family, login.host, revision(), &context, &store);
    NetworkRadioLoginTestAccess::clearSession();
    defer = true;
    loaded = false;
    loadNetworkRadioLogin(profile.family, &context, [&](NetworkRadioLogin restored) {
        loaded = true;
        require(!restored.savePassword && restored.password == QStringLiteral("new-session-secret"),
            "late vault completion cannot overwrite a newer session-only operator edit");
    }, &store);
    require(bool(pendingRead) && !loaded, "opt-in vault read is deferred by the injected store");
    login.savePassword = false;
    login.password = QStringLiteral("new-session-secret");
    stageNetworkRadioLogin(profile.family, login);
    pendingRead(QStringLiteral("old-vault-secret"));
    require(loaded, "late completion resolves to the newer staged login");
    NetworkRadioLoginTestAccess::clearSession();
    bool deadCallback = false;
    auto mortal = std::make_unique<QObject>();
    loadNetworkRadioLogin(profile.family, mortal.get(), [&](NetworkRadioLogin) { deadCallback = true; }, &store);
    mortal.reset();
    pendingRead(QStringLiteral("discarded-secret"));
    require(!deadCallback, "destroyed credential-load contexts receive no callback");
    NetworkRadioLoginTestAccess::clearSession();
    settings.remove(profile.settingsKey);
    settings.save();
}

void verifyConsentFormOwnership(QObject& context)
{
    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    ConnectionPanel panel;
    std::function<void(NetworkRadioLogin)> pending;
    ConnectionPanelStartupTestAccess::loader(panel, pending);
    QLineEdit* address = field<QLineEdit>(panel, "connectionManualIp");
    address->setText(QStringLiteral("radio-consent.example.invalid"));
    family(panel, "kenwood");
    QCheckBox* consent = field<QCheckBox>(panel, "connectionNetworkSavePassword");
    require(!consent->isChecked(), "a new network form starts session-only before a delayed load");
    consent->setChecked(true);
    consent->setChecked(false);
    NetworkRadioLogin incoming = saved();
    incoming.host = address->text();
    incoming.savePassword = true;
    pending(incoming);
    require(!consent->isChecked(), "delayed saved consent cannot overwrite an explicit operator opt-out");
    QLineEdit* username = field<QLineEdit>(panel, "connectionNetworkUsername");
    QSpinBox* port = field<QSpinBox>(panel, "connectionNetworkControlPort");
    QCheckBox* administrator = field<QCheckBox>(panel, "connectionNetworkAdministrator");
    consent->setChecked(true);
    edited(address, QStringLiteral("other-radio.example.invalid"));
    require(!consent->isChecked(), "editing an endpoint cannot inherit another host's password-save consent");
    consent->setChecked(true);
    port->setValue(port->value() + 1);
    require(!consent->isChecked(), "editing TCP port retires inherited password-save consent");
    consent->setChecked(true);
    administrator->setChecked(!administrator->isChecked());
    require(!consent->isChecked(), "editing account role retires inherited password-save consent");
    consent->setChecked(true);
    edited(username, QStringLiteral("new-account"));
    require(!consent->isChecked(), "editing username retires inherited password-save consent");
    edited(field<QLineEdit>(panel, "connectionNetworkPassword"), QStringLiteral("explicit-endpoint-secret"));
    require(field<QLineEdit>(panel, "connectionNetworkPassword")->echoMode() == QLineEdit::Password,
        "network password remains masked after changing its storage choice");
    std::function<void(const QHostInfo&)> resolved;
    int cancelled = 0;
    int connections = 0;
    ConnectionPanelStartupTestAccess::lookup(panel, resolved, cancelled);
    RadioInfo requested;
    QObject::connect(&panel, &ConnectionPanel::connectRequested, &context, [&](const RadioInfo& info) {
        ++connections;
        requested = info;
    });
    consent->setChecked(true);
    ConnectionPanelStartupTestAccess::connect(panel);
    require(bool(resolved), "explicit consent is captured by the pending connection lookup");
    consent->setChecked(false);
    QHostInfo answer;
    answer.setAddresses({QHostAddress(QStringLiteral("192.0.2.90"))});
    resolved(answer);
    require(connections == 0 && cancelled == 1,
        "editing consent fences an in-flight address lookup without starting an obsolete connection");
    consent->setChecked(true);
    ConnectionPanelStartupTestAccess::connect(panel);
    resolved(answer);
    require(connections == 1, "operator can opt in deliberately for the newly edited endpoint");
    RadioConnectRequest request;
    request.host = requested.address.toString();
    request.port = requested.port;
    populateNetworkRadioParams(request, QStringLiteral("kenwood"));
    const quint64 revision = request.params.value(QStringLiteral("connection.loginRevision")).toULongLong();
    require(revision != 0, "handed-off connection has a staged endpoint-bound credential revision");
    consent->setChecked(false);
    int writes = 0;
    NetworkRadioCredentialStore store;
    store.write = [&](const QString&, const QString&, QObject*) { ++writes; };
    commitNetworkRadioLogin(QStringLiteral("kenwood"), request.host, revision, &context, &store);
    require(writes == 0, "opt-out after connection handoff retires earlier password-write authorization");
    family(panel, "flex");
    require(!consent->isVisibleTo(&panel), "network password-storage control is scoped to network profiles");
    NetworkRadioLoginTestAccess::clearSession();
}

void verifyInheritedPasswordIdentity()
{
    for (int component = 0; component < 5; ++component) {
        AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
        ConnectionPanel panel;
        std::function<void(NetworkRadioLogin)> pending;
        ConnectionPanelStartupTestAccess::loader(panel, pending);
        QLineEdit* host = field<QLineEdit>(panel, "connectionManualIp");
        host->setText(saved().host);
        family(panel, "kenwood");
        NetworkRadioLogin incoming = saved();
        incoming.savePassword = true;
        pending(incoming);
        QLineEdit* password = field<QLineEdit>(panel, "connectionNetworkPassword");
        QCheckBox* consent = field<QCheckBox>(panel, "connectionNetworkSavePassword");
        require(password->text() == incoming.password && consent->isChecked(),
            "saved password and consent fill only an exactly matched endpoint/account identity");
        if (component == 4) { edited(password, QStringLiteral("operator-session-secret")); }
        if (component == 0 || component == 4) { edited(host, QStringLiteral("192.0.2.99")); }
        if (component == 1) {
            QSpinBox* port = field<QSpinBox>(panel, "connectionNetworkControlPort");
            port->setValue(port->value() + 1);
        }
        if (component == 2) {
            QCheckBox* role = field<QCheckBox>(panel, "connectionNetworkAdministrator");
            role->setChecked(!role->isChecked());
        }
        if (component == 3) { edited(field<QLineEdit>(panel, "connectionNetworkUsername"), QStringLiteral("another-account")); }
        require(!consent->isChecked(), "endpoint/account edits retire inherited save authorization");
        require(password->text() == (component == 4 ? QStringLiteral("operator-session-secret") : QString()),
            "identity edits clear inherited passwords while preserving an explicitly typed session password");
        pending(incoming);
        require(password->text() == (component == 4 ? QStringLiteral("operator-session-secret") : QString()),
            "late saved-password completion cannot refill a changed endpoint identity");
    }
    for (bool differentPort : {false, true}) {
        AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
        ConnectionPanel panel;
        std::function<void(NetworkRadioLogin)> pending;
        ConnectionPanelStartupTestAccess::loader(panel, pending);
        field<QLineEdit>(panel, "connectionManualIp")->setText(differentPort ? saved().host : QStringLiteral("192.0.2.99"));
        family(panel, "kenwood");
        if (differentPort) { field<QSpinBox>(panel, "connectionNetworkControlPort")->setValue(62001); }
        NetworkRadioLogin incoming = saved(); incoming.savePassword = true;
        pending(incoming);
        require(field<QLineEdit>(panel, "connectionNetworkPassword")->text().isEmpty()
            && !field<QCheckBox>(panel, "connectionNetworkSavePassword")->isChecked(),
            "a different host or TCP port cannot inherit a loaded family password or its consent");
    }
    NetworkRadioLoginTestAccess::clearSession();
}

}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("connection-panel-network-login"));
    require(profile.isValid(), "isolated settings");
    QApplication app(argc, argv);
    AppSettings::instance().load();
    verifyCredentialConsent(app);
    verifyConsentFormOwnership(app);
    verifyInheritedPasswordIdentity();
    const NetworkRadioProfile network = *networkRadioProfile(QStringLiteral("kenwood"));
    NetworkRadioLogin original = saved();
    original.host = QStringLiteral("radio-a.example");
    const QString originalKey = networkRadioCredentialKey(network, original);
    QMap<QString, QString> vault{{originalKey, original.password}};
    for (int component = 0; component < 4; ++component) {
        NetworkRadioLogin next = original;
        if (component == 0) { next.host = QStringLiteral("radio-b.example"); }
        if (component == 1) { next.username = QStringLiteral("other-user"); }
        if (component == 2) { ++next.controlPort; }
        if (component == 3) { next.administrator = !next.administrator; }
        // Metadata can already describe the new connection when its vault write
        // fails. Reloading that identity must not find the old entry.
        require(vault.value(networkRadioCredentialKey(network, next)).isEmpty(),
            "failed credential save cannot reuse another endpoint or account password");
    }
    NetworkRadioLogin equivalent = original;
    equivalent.host = QStringLiteral(" RADIO-A.EXAMPLE ");
    equivalent.password = QStringLiteral("replacement-secret");
    require(networkRadioCredentialKey(network, equivalent) == originalKey,
        "DNS case and password replacement preserve the endpoint account entry");
    NetworkRadioLogin delimited = original;
    delimited.host = QStringLiteral("radio");
    delimited.username = QStringLiteral("a.example:stored-user");
    require(networkRadioCredentialKey(network, delimited) != originalKey,
        "structured identity prevents field-boundary aliasing");
    require(!originalKey.contains(original.host) && !originalKey.contains(original.username)
        && !originalKey.contains(original.password), "vault key names do not embed connection identity or secrets");
    const auto populated = [&network](const NetworkRadioLogin& login, const QString& host, quint16 port) {
        stageNetworkRadioLogin(network.family, login);
        RadioConnectRequest request;
        request.host = host;
        request.port = port;
        populateNetworkRadioParams(request, network.family);
        return request.params;
    };
    NetworkRadioLogin endpoint = original;
    endpoint.host = QStringLiteral("192.0.2.5");
    endpoint.controlPort = network.controlPort;
    for (const quint16 port : {quint16(0), network.controlPort}) {
        const QVariantMap params = populated(endpoint, endpoint.host, port);
        require(params.value(QStringLiteral("kenwood.username")) == endpoint.username
            && params.value(QStringLiteral("kenwood.password")) == endpoint.password
            && params.value(QStringLiteral("kenwood.administrator")).toBool() == endpoint.administrator
            && params.value(QStringLiteral("connection.loginRevision")).toULongLong() != 0,
            "implicit and explicit default ports consume the matching endpoint login");
    }
    require(populated(endpoint, endpoint.host, 62000).isEmpty(),
        "another TCP port cannot consume default-port credentials or login revision");
    endpoint.controlPort = 62000;
    const QVariantMap mapped = populated(endpoint, endpoint.host, endpoint.controlPort);
    require(mapped.value(QStringLiteral("kenwood.password")) == endpoint.password
        && mapped.value(QStringLiteral("connection.loginRevision")).toULongLong() != 0,
        "a matching mapped TCP port consumes its own endpoint login");
    for (const quint16 port : {quint16(0), network.controlPort, quint16(62001)}) {
        require(populated(endpoint, endpoint.host, port).isEmpty(),
            "mapped-port credentials cannot follow an implicit default or another explicit port");
    }
    require(populated(endpoint, QStringLiteral("192.0.2.6"), endpoint.controlPort).isEmpty(),
        "a matching port on another host cannot consume credentials or login revision");
    std::function<void(NetworkRadioLogin)> pending;
    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    ConnectionPanel panel;
    ConnectionPanelStartupTestAccess::loader(panel, pending);
    family(panel, "kenwood");
    require(bool(pending), "credential read is deferred by the injected loader");
    QLineEdit* user = field<QLineEdit>(panel, "connectionNetworkUsername");
    QLineEdit* password = field<QLineEdit>(panel, "connectionNetworkPassword");
    QSpinBox* port = field<QSpinBox>(panel, "connectionNetworkControlPort");
    QCheckBox* administrator = field<QCheckBox>(panel, "connectionNetworkAdministrator");
    QComboBox* variant = field<QComboBox>(panel, "connectionNetworkVariant");
    QCheckBox* consent = field<QCheckBox>(panel, "connectionNetworkSavePassword");
    require(!consent->isChecked() && !consent->accessibleDescription().isEmpty(),
        "network password saving is off by default with an accessible session-only explanation");
    edited(user, QStringLiteral("typed-user"));
    edited(user, QString());
    edited(password, QStringLiteral("typed-secret"));
    edited(password, QString());
    const int originalPort = port->value();
    port->setValue(originalPort + 1);
    port->setValue(originalPort);
    administrator->setChecked(true);
    administrator->setChecked(false);
    variant->setCurrentIndex(variant->findData(QStringLiteral("k")));
    QMetaObject::invokeMethod(variant, "activated", Qt::DirectConnection, Q_ARG(int, variant->currentIndex()));
    pending(saved());
    require(user->text().isEmpty() && password->text().isEmpty() && port->value() == originalPort
        && !administrator->isChecked() && variant->currentData() == QStringLiteral("k"),
        "delayed saved login cannot overwrite operator edits returned to initial values");

    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    ConnectionPanel untouched;
    ConnectionPanelStartupTestAccess::loader(untouched, pending);
    field<QLineEdit>(untouched, "connectionManualIp")->setText(saved().host);
    family(untouched, "kenwood");
    pending(saved());
    require(field<QLineEdit>(untouched, "connectionNetworkUsername")->text() == saved().username
        && field<QLineEdit>(untouched, "connectionNetworkPassword")->text() == saved().password
        && field<QSpinBox>(untouched, "connectionNetworkControlPort")->value() == 62000
        && field<QCheckBox>(untouched, "connectionNetworkAdministrator")->isChecked(),
        "untouched fields still receive saved connection details");

    std::function<void(const QHostInfo&)> resolved;
    int cancelled = 0;
    int connections = 0;
    ConnectionPanelStartupTestAccess::lookup(untouched, resolved, cancelled);
    QObject::connect(&untouched, &ConnectionPanel::connectRequested, &app, [&](const RadioInfo&) { ++connections; });
    field<QLineEdit>(untouched, "connectionManualIp")->setText(QStringLiteral("radio.example.invalid"));
    require(field<QLineEdit>(untouched, "connectionNetworkPassword")->text().isEmpty(),
        "changing the host clears the inherited saved password before a new connection");
    edited(field<QLineEdit>(untouched, "connectionNetworkPassword"), QStringLiteral("typed-endpoint-secret"));
    ConnectionPanelStartupTestAccess::connect(untouched);
    require(bool(resolved), "manual hostname starts only the injected resolver");
    QComboBox* source = field<QComboBox>(untouched, "connectionManualSourcePath");
    QMetaObject::invokeMethod(source, "activated", Qt::DirectConnection, Q_ARG(int, 0));
    QHostInfo answer;
    answer.setAddresses({QHostAddress(QStringLiteral("192.0.2.5"))});
    resolved(answer);
    require(cancelled == 1 && connections == 0, "operator source selection cancels pending DNS and fences its stale completion");
    ConnectionPanelStartupTestAccess::connect(untouched);
    resolved(answer);
    require(connections == 1, "a fresh deliberate attempt can connect after cancellation");
    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    ConnectionPanel stale;
    ConnectionPanelStartupTestAccess::loader(stale, pending);
    family(stale, "kenwood");
    family(stale, "flex");
    pending(saved());
    require(field<QLineEdit>(stale, "connectionNetworkPassword")->text().isEmpty(),
        "saved credentials cannot populate a retired family selection");
    AppSettings::instance().setValue("ConnectByIpRadioFamily", "flex");
    std::unique_ptr<ConnectionPanel> mortal = std::make_unique<ConnectionPanel>();
    ConnectionPanelStartupTestAccess::loader(*mortal, pending);
    family(*mortal, "kenwood");
    mortal.reset();
    pending(saved());
    require(AppSettings::instance().value("RoutedProfilesJson").toString().isEmpty(),
        "unproven authenticated endpoint cannot replace a saved profile");
    untouched.setConnected(true);
    const auto profiles = [] {
        return QJsonDocument::fromJson(AppSettings::instance().value("RoutedProfilesJson").toString().toUtf8()).object();
    };
    for (const QString& host : {QStringLiteral("radio.example.invalid"), QStringLiteral("192.0.2.5")}) {
        const QJsonObject savedProfile = profiles().value(host).toObject();
        require(savedProfile.value("identity").toObject().value("family").toString() == QLatin1String("kenwood")
            && savedProfile.value("bind").toObject().value("mode").toString() == QLatin1String("auto")
            && savedProfile.value("network").toObject().value("control_port").toInt() == 62000,
            "successful authentication saves family, route and TCP port under hostname and resolved address");
    }
    family(untouched, "flex");
    ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.5"));
    require(field<QComboBox>(untouched, "connectionManualRadioType")->currentData() == QStringLiteral("kenwood"),
        "selecting a recent address restores its authenticated network family");
    family(untouched, "flex");
    {
        ConnectionPanel reopened;
        ConnectionPanelStartupTestAccess::loader(reopened, pending);
        ConnectionPanelStartupTestAccess::restore(reopened, QStringLiteral("radio.example.invalid"));
        require(field<QComboBox>(reopened, "connectionManualRadioType")->currentData() == QStringLiteral("kenwood"),
            "reconstructed panel restores hostname family from the routed profile");
    }
    family(untouched, "kenwood");
    untouched.setConnected(false);
    field<QLineEdit>(untouched, "connectionManualIp")->setText(QStringLiteral("192.0.2.6"));
    const int beforeFailedAttempt = connections;
    ConnectionPanelStartupTestAccess::connect(untouched);
    require(connections == beforeFailedAttempt + 1, "failed-authentication case dispatches a real connection request");
    untouched.setConnected(false);
    untouched.setConnected(true);
    require(!profiles().contains(QStringLiteral("192.0.2.6")),
        "a failed connection cannot commit later on an unrelated success");

    QSpinBox* endpointPort = field<QSpinBox>(untouched, "connectionNetworkControlPort");
    QCheckBox* endpointConsent = field<QCheckBox>(untouched, "connectionNetworkSavePassword");
    for (const auto& endpoint : QList<QPair<QString, int>>{{QStringLiteral("192.0.2.10"), 60000},
                                                        {QStringLiteral("192.0.2.11"), 62000}}) {
        untouched.setConnected(false);
        endpointPort->setValue(endpoint.second);
        field<QLineEdit>(untouched, "connectionManualIp")->setText(endpoint.first);
        const bool savePassword = endpoint.second == 62000;
        endpointConsent->setChecked(savePassword);
        const int beforeEndpointAttempt = connections;
        ConnectionPanelStartupTestAccess::connect(untouched);
        require(connections == beforeEndpointAttempt + 1, "each recent endpoint dispatches its own connection request");
        {
            const QSignalBlocker blocker(endpointPort);
            endpointPort->setValue(65000);
            const QSignalBlocker consentBlocker(endpointConsent);
            endpointConsent->setChecked(!savePassword);
        }
        untouched.setConnected(true);
        require(profiles().value(endpoint.first).toObject().value("network").toObject().value("control_port").toInt()
            == endpoint.second, "profile commits the authenticated request port, not callback-time widget state");
        require(profiles().value(endpoint.first).toObject().value("network").toObject().value("save_password").toBool()
            == savePassword, "profile commits captured consent rather than callback-time checkbox state");
    }
    ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.10"));
    require(endpointPort->value() == 60000 && !endpointConsent->isChecked(),
        "first recent endpoint restores its own TCP port and session-only choice");
    ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.11"));
    require(endpointPort->value() == 62000 && endpointConsent->isChecked(),
        "second recent endpoint restores its own port and explicit password consent");
    family(untouched, "flex");
    ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.10"));
    require(bool(pending), "family switch starts a deferred login loader");
    pending(saved());
    require(endpointPort->value() == 60000, "late family-wide login cannot overwrite the chosen endpoint port");
    endpointPort->setValue(61000);
    pending(saved());
    require(endpointPort->value() == 61000, "late login cannot overwrite an operator's later port edit");
    ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.11"), false);
    require(endpointPort->value() == 61000, "typing an address does not restore its port over an operator edit");

    for (const QJsonValue& invalidPort : {QJsonValue(), QJsonValue(-1), QJsonValue(65536),
             QJsonValue(QStringLiteral("62000")), QJsonValue(62000.5)}) {
        QJsonObject allProfiles = profiles();
        QJsonObject invalidProfile = allProfiles.value(QStringLiteral("192.0.2.11")).toObject();
        QJsonObject networkMetadata = invalidProfile.value("network").toObject();
        networkMetadata.insert("control_port", invalidPort);
        networkMetadata.insert("save_password", true);
        invalidProfile.insert("network", networkMetadata);
        allProfiles.insert(QStringLiteral("192.0.2.12"), invalidProfile);
        AppSettings::instance().setValue("RoutedProfilesJson",
            QString::fromUtf8(QJsonDocument(allProfiles).toJson(QJsonDocument::Compact)));
        endpointConsent->setChecked(true);
        const int beforePort = endpointPort->value();
        ConnectionPanelStartupTestAccess::restore(untouched, QStringLiteral("192.0.2.12"));
        require(endpointPort->value() == beforePort && !endpointConsent->isChecked(),
            "missing or malformed endpoint ports cannot apply saved consent to a previous port");
    }

    std::cout << "Network connection form ownership checks passed\n";
}
