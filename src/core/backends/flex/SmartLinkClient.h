#pragma once

#include <QObject>
#include <QList>
#include <QSslSocket>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QNetworkAccessManager>

namespace AetherSDR {

// Information about a radio discovered via SmartLink (WAN).
struct WanRadioInfo {
    QString serial;
    QString nickname;
    QString callsign;
    QString model;
    QString status;         // "Available", "In Use", etc.
    QString publicIp;
    int     publicTlsPort{-1};
    int     publicUdpPort{-1};
    bool    upnpSupported{false};
    bool    requiresHolePunch{false};
    int     licensedClients{1};
    QString maxLicensedVersion;
    QString guiClientPrograms;
    QString guiClientStations;
    QString guiClientHandles;
    QString guiClientIps;
    QString guiClientHosts;
};

// SmartLink client — manages the TLS connection to smartlink.flexradio.com
// and Auth0 authentication for remote radio access.
//
// Usage:
//   SmartLinkClient client;
//   connect(&client, &SmartLinkClient::authenticated, ...);
//   connect(&client, &SmartLinkClient::radioListReceived, ...);
//   client.login("user@example.com", "password");
//
class SmartLinkClient : public QObject {
    Q_OBJECT

public:
    explicit SmartLinkClient(QObject* parent = nullptr);
    ~SmartLinkClient() override;

    // Auth0 login with email/password → obtains JWT token
    void login(const QString& email, const QString& password);
    // Login with a previously saved refresh token
    void loginWithRefreshToken(const QString& refreshToken);
    // Logout and disconnect from SmartLink server
    void logout();
    // Attempt silent login from stored keychain credentials
    void tryAutoLogin();
    // Save current refresh token to OS keychain
    void saveCredentials();
    // Clear stored credentials from OS keychain
    void clearCredentials();

    // Request connection to a specific radio (by serial).
    // holePunchPort is the local UDP port we've bound for VITA-49 reception;
    // the SmartLink server tells the radio to send UDP to our public IP:port.
    void requestConnect(const QString& serial, quint16 holePunchPort = 0);
    // Ask the SmartLink server to force-disconnect specific GUI clients.
    void disconnectRadioClients(const QString& serial, const QList<quint32>& handles);
    // Disconnect from SmartLink server
    void disconnect();
    // Reconnect to SmartLink using the current authenticated token.
    void reconnect();

    bool isConnected()     const { return m_serverConnected; }
    bool isAuthenticated() const { return m_authenticated; }
    QString publicIp()     const { return m_publicIp; }
    QString callsign()     const { return m_userCallsign; }
    QString firstName()    const { return m_userFirstName; }
    QString lastName()     const { return m_userLastName; }
    QString idToken()      const { return m_idToken; }
    QString refreshToken() const { return m_refreshToken; }

signals:
    // Auth0 login result
    void authenticated();
    void authFailed(const QString& error);

    // SmartLink server events
    void serverConnected();
    void serverDisconnected();
    void radioListReceived(const QList<WanRadioInfo>& radios);
    void connectReady(const QString& wanHandle, const QString& serial);
    void testConnectionResult(const QString& serial, bool upnpTcp, bool upnpUdp,
                              bool fwdTcp, bool fwdUdp, bool holePunch);

private slots:
    void onSslConnected();
    void onSslDisconnected();
    void onSslError(const QList<QSslError>& errors);
    void onReadyRead();
    void onPingTimer();

private:
    void connectToServer();
    void parseMessage(const QString& msg);
    void parseRadioList(const QString& msg);
    void parseConnectReady(const QString& msg);
    void parseApplicationInfo(const QString& msg);
    void parseUserSettings(const QString& msg);
    void parseTestResults(const QString& msg);

    // Auth0
    static constexpr const char* AUTH0_DOMAIN   = "frtest.auth0.com";
    static constexpr const char* AUTH0_CLIENT_ID = "4Y9fEIIsVYyQo5u6jr7yBWc4lV5ugC2m";
    static constexpr const char* SMARTLINK_HOST  = "smartlink.flexradio.com";
    static constexpr int         SMARTLINK_PORT  = 443;

    QNetworkAccessManager m_nam;
    QString m_idToken;
    QString m_refreshToken;
    bool    m_authenticated{false};
    bool    m_authRequestInProgress{false};

    // SmartLink server TLS connection
    // NOTE: m_pingTimer and m_serverConnected must be declared before m_socket
    // so they outlive it during destruction. QSslSocket's destructor can emit
    // disconnected(), which invokes onSslDisconnected() → m_pingTimer.stop().
    QTimer     m_pingTimer;
    bool       m_serverConnected{false};
    QSslSocket m_socket;
    QByteArray m_readBuffer;

    // User info from server
    QString m_publicIp;
    QString m_userCallsign;
    QString m_userFirstName;
    QString m_userLastName;
};

} // namespace AetherSDR
