#pragma once

#include "core/backends/RemoteRadioAccount.h"
#include <QObject>
#include <QVector>
#include <optional>

namespace AetherSDR {
class SmartLinkClient;

// Engine-owned account state and operator intents shared by desktop and daemon.
// The UI sees discovery snapshots, never authentication tokens or TLS sockets.
class SmartLinkAccountModel final : public QObject {
    Q_OBJECT
    friend struct SmartLinkAccountModelTestAccess;
public:
    explicit SmartLinkAccountModel(QObject* parent = nullptr);
    ~SmartLinkAccountModel() override;
    void login(const QString& email, const QString& password);
    void logout();
    void tryAutoLogin();
    void reconnect();
    void forgetAccount(const QString& id);
    void setAccountId(const QString& id);
    QString accountId() const;
    bool accountLocked() const;
    bool isConnected() const;
    bool isAuthenticated() const;
    QString callsign() const;
    QString firstName() const;
    QString lastName() const;
    const QList<WanRadioInfo>& radios() const { return m_radios; }
    bool disconnectRadioClients(const QString& serial, const QList<quint32>& handles);
    std::optional<RemoteRadioRequest> connectionRequest(const QString& serial,
        const QList<quint32>& disconnectHandles = {}) const;
    static bool persistentStoreAvailable();
    static QVector<PinnedCertInfo> pinnedCertificates();
    static void forgetPinnedCertificate(const QString& host);
    static void forgetAllPinnedCertificates();

signals:
    void authenticated();
    void authFailed(const QString& error);
    void serverConnected();
    void serverDisconnected();
    void radioListReceived(const QList<AetherSDR::WanRadioInfo>& radios);
private:
    std::shared_ptr<SmartLinkClient> m_client;
    QList<WanRadioInfo> m_radios;
};
} // namespace AetherSDR
