#include "models/SmartLinkAccountModel.h"
#include "core/backends/flex/SmartLinkClient.h"
#include "core/backends/flex/WanConnection.h"

namespace AetherSDR {
SmartLinkAccountModel::SmartLinkAccountModel(QObject* parent)
    : QObject(parent), m_client(std::make_shared<SmartLinkClient>())
{
    connect(m_client.get(), &SmartLinkClient::authenticated, this, &SmartLinkAccountModel::authenticated);
    connect(m_client.get(), &SmartLinkClient::authFailed, this, &SmartLinkAccountModel::authFailed);
    connect(m_client.get(), &SmartLinkClient::serverConnected, this, &SmartLinkAccountModel::serverConnected);
    connect(m_client.get(), &SmartLinkClient::serverDisconnected, this, &SmartLinkAccountModel::serverDisconnected);
    connect(m_client.get(), &SmartLinkClient::radioListReceived, this, [this](const QList<WanRadioInfo>& radios) {
        m_radios = radios;
        emit radioListReceived(m_radios);
    });
}
SmartLinkAccountModel::~SmartLinkAccountModel() = default;
void SmartLinkAccountModel::login(const QString& email, const QString& password) { m_client->login(email, password); }
void SmartLinkAccountModel::logout() { m_client->logout(); }
void SmartLinkAccountModel::tryAutoLogin() { m_client->tryAutoLogin(); }
void SmartLinkAccountModel::reconnect() { m_client->reconnect(); }
void SmartLinkAccountModel::forgetAccount(const QString& id) { m_client->forgetAccount(id); }
void SmartLinkAccountModel::setAccountId(const QString& id) { m_client->setAccountId(id); }
QString SmartLinkAccountModel::accountId() const { return m_client->accountId(); }
bool SmartLinkAccountModel::accountLocked() const { return m_client->accountLocked(); }
bool SmartLinkAccountModel::isConnected() const { return m_client->isConnected(); }
bool SmartLinkAccountModel::isAuthenticated() const { return m_client->isAuthenticated(); }
QString SmartLinkAccountModel::callsign() const { return m_client->callsign(); }
QString SmartLinkAccountModel::firstName() const { return m_client->firstName(); }
QString SmartLinkAccountModel::lastName() const { return m_client->lastName(); }
bool SmartLinkAccountModel::persistentStoreAvailable() { return SmartLinkClient::persistentStoreAvailable(); }
std::optional<RemoteRadioRequest> SmartLinkAccountModel::connectionRequest(
    const QString& serial, const QList<quint32>& handles) const
{
    if (!isAuthenticated() || accountLocked()) { return {}; }
    for (const WanRadioInfo& radio : m_radios) {
        if (radio.serial == serial) { return RemoteRadioRequest{m_client, radio, handles}; }
    }
    return {};
}
bool SmartLinkAccountModel::disconnectRadioClients(const QString& serial, const QList<quint32>& handles)
{
    if (!isConnected()) { return false; }
    for (const WanRadioInfo& radio : m_radios) {
        if (radio.serial == serial) { m_client->disconnectRadioClients(serial, handles); return true; }
    }
    return false;
}
QVector<PinnedCertInfo> SmartLinkAccountModel::pinnedCertificates() { return WanCertCache::listPinnedCerts(); }
void SmartLinkAccountModel::forgetPinnedCertificate(const QString& host) { WanCertCache::forgetPinnedCert(host); }
void SmartLinkAccountModel::forgetAllPinnedCertificates() { WanCertCache::forgetAllPinnedCerts(); }
} // namespace AetherSDR
