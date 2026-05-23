#include "SmartLinkClient.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QUrl>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

// Cap the line-assembly buffer.  A buggy or hostile SmartLink discovery/auth
// peer that dribbles bytes without ever sending '\n' would otherwise grow
// m_readBuffer unbounded until QByteArray refuses to allocate (process OOM).
// SmartLink auth messages are tens of bytes; 16 MiB is wildly larger than any
// legitimate handshake burst.  Same pattern as WanConnection /
// RadioConnection / DxClusterClient (issue #2955) and GHSA-7w4w-wfqm-wh93
// (M2, RigctlServer).
static constexpr int kMaxReadBuffer = 16 * 1024 * 1024;

SmartLinkClient::SmartLinkClient(QObject* parent)
    : QObject(parent)
{
    // SmartLink server TLS socket
    connect(&m_socket, &QSslSocket::connected,    this, &SmartLinkClient::onSslConnected);
    connect(&m_socket, &QSslSocket::disconnected, this, &SmartLinkClient::onSslDisconnected);
    connect(&m_socket, &QSslSocket::readyRead,    this, &SmartLinkClient::onReadyRead);
    connect(&m_socket, &QSslSocket::sslErrors,    this, &SmartLinkClient::onSslError);

    // Keepalive ping every 10 seconds (matches FlexLib SslClient)
    m_pingTimer.setInterval(10000);
    connect(&m_pingTimer, &QTimer::timeout, this, &SmartLinkClient::onPingTimer);
}

SmartLinkClient::~SmartLinkClient()
{
    // Prevent QSslSocket destructor from delivering signals into partially
    // destroyed state (e.g. disconnected → onSslDisconnected → m_pingTimer.stop
    // on a dead timer). See issue #842.
    QObject::disconnect(&m_socket, nullptr, this, nullptr);
    disconnect();
}

// ── Auth0 Login ──────────────────────────────────────────────────────────────

void SmartLinkClient::login(const QString& email, const QString& password)
{
    // Auth0 Resource Owner Password Grant
    // https://auth0.com/docs/get-started/authentication-and-authorization-flow/resource-owner-password-flow
    QUrl url(QString("https://%1/oauth/token").arg(AUTH0_DOMAIN));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["grant_type"]  = "http://auth0.com/oauth/grant-type/password-realm";
    body["realm"]       = "Username-Password-Authentication";
    body["username"]    = email;
    body["password"]    = password;
    body["client_id"]   = AUTH0_CLIENT_ID;
    body["scope"]       = "openid email given_name family_name profile picture offline_access";

    qCDebug(lcSmartLink) << "SmartLinkClient: Auth0 login for" << email.left(3) + "***";

    // Remember email (Base64-encoded) for pre-fill on next launch
    auto& s = AppSettings::instance();
    s.setValue("SmartLinkEmail", QString::fromUtf8(email.toUtf8().toBase64()));
    s.save();

    auto* reply = m_nam.post(req, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const auto data = reply->readAll();
            QJsonObject err = QJsonDocument::fromJson(data).object();
            QString desc = err.value("error_description").toString(reply->errorString());
            qCWarning(lcSmartLink) << "SmartLinkClient: Auth0 login failed:" << desc;
            emit authFailed(desc);
            return;
        }

        const auto data = reply->readAll();
        QJsonObject resp = QJsonDocument::fromJson(data).object();
        m_idToken      = resp.value("id_token").toString();
        m_refreshToken = resp.value("refresh_token").toString();

        if (m_idToken.isEmpty()) {
            qCWarning(lcSmartLink) << "SmartLinkClient: Auth0 response missing id_token";
            emit authFailed("No id_token in response");
            return;
        }

        qCDebug(lcSmartLink) << "SmartLinkClient: Auth0 login successful, id_token length:" << m_idToken.length();
        m_authenticated = true;
        saveCredentials();
        emit authenticated();

        // Now connect to SmartLink server with the token
        connectToServer();
    });
}

void SmartLinkClient::loginWithRefreshToken(const QString& refreshToken)
{
    if (m_authRequestInProgress) {
        qCDebug(lcSmartLink) << "SmartLinkClient: Auth0 refresh already in progress";
        return;
    }

    QUrl url(QString("https://%1/oauth/token").arg(AUTH0_DOMAIN));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["grant_type"]    = "refresh_token";
    body["refresh_token"] = refreshToken;
    body["client_id"]     = AUTH0_CLIENT_ID;

    qCDebug(lcSmartLink) << "SmartLinkClient: Auth0 refresh token login";

    m_authRequestInProgress = true;
    auto* reply = m_nam.post(req, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_authRequestInProgress = false;

        if (reply->error() != QNetworkReply::NoError) {
            const auto data = reply->readAll();
            QJsonObject err = QJsonDocument::fromJson(data).object();
            QString desc = err.value("error_description").toString(reply->errorString());
            qCWarning(lcSmartLink) << "SmartLinkClient: Auth0 refresh failed:" << desc;
            emit authFailed(desc);
            return;
        }

        const auto data = reply->readAll();
        QJsonObject resp = QJsonDocument::fromJson(data).object();
        m_idToken = resp.value("id_token").toString();
        // Auth0 may return a new refresh token
        QString newRefresh = resp.value("refresh_token").toString();
        if (!newRefresh.isEmpty())
            m_refreshToken = newRefresh;

        if (m_idToken.isEmpty()) {
            emit authFailed("No id_token in refresh response");
            return;
        }

        qCDebug(lcSmartLink) << "SmartLinkClient: Auth0 refresh successful";
        m_authenticated = true;
        saveCredentials();
        emit authenticated();
        connectToServer();
    });
}

void SmartLinkClient::logout()
{
    m_authenticated = false;
    m_idToken.clear();
    m_refreshToken.clear();
    m_userCallsign.clear();
    m_userFirstName.clear();
    m_userLastName.clear();
    clearCredentials();
    disconnect();
}

// ── Credential Persistence ──────────────────────────────────────────────────

void SmartLinkClient::saveCredentials()
{
#ifdef HAVE_KEYCHAIN
    if (m_refreshToken.isEmpty()) return;

    auto* job = new QKeychain::WritePasswordJob("AetherSDR");
    job->setAutoDelete(true);
    job->setKey("smartlink_refresh_token");
    job->setTextData(m_refreshToken);
    connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError)
            qCWarning(lcSmartLink) << "SmartLinkClient: keychain save failed:" << j->errorString();
        else
            qCDebug(lcSmartLink) << "SmartLinkClient: refresh token saved to keychain";
    });
    job->start();
#endif
}

void SmartLinkClient::clearCredentials()
{
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::DeletePasswordJob("AetherSDR");
    job->setAutoDelete(true);
    job->setKey("smartlink_refresh_token");
    connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError && j->error() != QKeychain::EntryNotFound)
            qCWarning(lcSmartLink) << "SmartLinkClient: keychain clear failed:" << j->errorString();
        else
            qCDebug(lcSmartLink) << "SmartLinkClient: refresh token cleared from keychain";
    });
    job->start();
#endif
    AppSettings::instance().remove("SmartLinkEmail");
    AppSettings::instance().save();
}

void SmartLinkClient::tryAutoLogin()
{
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob("AetherSDR");
    job->setAutoDelete(true);
    job->setKey("smartlink_refresh_token");
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* j) {
        auto* readJob = static_cast<QKeychain::ReadPasswordJob*>(j);
        if (j->error() != QKeychain::NoError || readJob->textData().isEmpty()) {
            qCDebug(lcSmartLink) << "SmartLinkClient: no stored credentials";
            return;
        }
        qCDebug(lcSmartLink) << "SmartLinkClient: found stored refresh token, attempting auto-login";
        loginWithRefreshToken(readJob->textData());
    });
    job->start();
#endif
}

// ── SmartLink Server Connection ──────────────────────────────────────────────

void SmartLinkClient::connectToServer()
{
    if (m_idToken.isEmpty()) {
        qCWarning(lcSmartLink) << "SmartLinkClient: cannot connect to server without token";
        return;
    }

    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        qCWarning(lcSmartLink) << "SmartLinkClient: connect requested while socket is not idle";
        return;
    }

    qCDebug(lcSmartLink) << "SmartLinkClient: connecting to" << SMARTLINK_HOST << ":" << SMARTLINK_PORT;

    // Standard TLS with certificate validation
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_socket.setSslConfiguration(sslConfig);

    m_socket.connectToHostEncrypted(SMARTLINK_HOST, SMARTLINK_PORT);
}

void SmartLinkClient::disconnect()
{
    m_pingTimer.stop();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.disconnectFromHost();
    }
    m_serverConnected = false;
}

void SmartLinkClient::reconnect()
{
    if (m_serverConnected) {
        return;
    }

    if (m_socket.state() == QAbstractSocket::ClosingState)
        m_socket.abort();

    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        qCDebug(lcSmartLink) << "SmartLinkClient: reconnect deferred until socket is idle";
        return;
    }

    if (!m_refreshToken.isEmpty()) {
        loginWithRefreshToken(m_refreshToken);
        return;
    }

    if (m_idToken.isEmpty()) {
        return;
    }

    connectToServer();
}

void SmartLinkClient::requestConnect(const QString& serial, quint16 holePunchPort)
{
    if (!m_serverConnected) return;
    QString cmd = QString("application connect serial=%1 hole_punch_port=%2\n")
                      .arg(serial).arg(holePunchPort);
    qCDebug(lcSmartLink) << "SmartLinkClient: requesting connection to" << serial
             << "hole_punch_port=" << holePunchPort;
    m_socket.write(cmd.toUtf8());
}

void SmartLinkClient::disconnectRadioClients(const QString& serial, const QList<quint32>& handles)
{
    if (!m_serverConnected || serial.isEmpty())
        return;

    QList<quint32> uniqueHandles;
    for (quint32 handle : handles) {
        if (handle != 0 && !uniqueHandles.contains(handle))
            uniqueHandles.append(handle);
    }

    for (quint32 handle : uniqueHandles) {
        const QString handleText = QString("0x%1").arg(handle, 0, 16);
        const QString cmd = QString("application disconnect_users serial=%1 handle=%2\n")
                                .arg(serial, handleText);
        qCDebug(lcSmartLink) << "SmartLinkClient: disconnecting GUI client"
                             << handleText << "from radio" << serial;
        m_socket.write(cmd.toUtf8());
    }
}

// ── TLS Socket Callbacks ─────────────────────────────────────────────────────

void SmartLinkClient::onSslConnected()
{
    qCDebug(lcSmartLink) << "SmartLinkClient: TLS connected to SmartLink server";
    m_serverConnected = true;

    // Register with the server
    qCDebug(lcSmartLink) << "SmartLinkClient: sending application register (token length:" << m_idToken.length() << ")";
    QString cmd = QString("application register name=AetherSDR platform=Linux token=%1\n")
                      .arg(m_idToken);
    m_socket.write(cmd.toUtf8());

    // Start keepalive pings
    m_pingTimer.start();
    qCDebug(lcSmartLink) << "SmartLinkClient: keepalive ping timer started (10s interval)";

    emit serverConnected();
}

void SmartLinkClient::onSslDisconnected()
{
    qCDebug(lcSmartLink) << "SmartLinkClient: TLS disconnected from SmartLink server";
    m_pingTimer.stop();
    m_serverConnected = false;
    emit serverDisconnected();
}

void SmartLinkClient::onSslError(const QList<QSslError>& errors)
{
    for (const auto& err : errors)
        qCWarning(lcSmartLink) << "SmartLinkClient: SSL error:" << err.errorString();
    // Don't ignore errors for the SmartLink server (it has a valid cert)
}

void SmartLinkClient::onReadyRead()
{
    m_readBuffer.append(m_socket.readAll());
    if (m_readBuffer.size() > kMaxReadBuffer) {
        qCWarning(lcSmartLink) << "SmartLinkClient: read buffer exceeded"
                               << kMaxReadBuffer << "bytes without newline — disconnecting";
        m_socket.disconnectFromHost();
        // disconnectFromHost() is async on TLS sockets; clear the buffer so a
        // stale onReadyRead() before the disconnect completes can't trip the
        // cap again.
        m_readBuffer.clear();
        return;
    }

    while (true) {
        int idx = m_readBuffer.indexOf('\n');
        if (idx < 0) break;

        QString line = QString::fromUtf8(m_readBuffer.left(idx)).trimmed();
        m_readBuffer.remove(0, idx + 1);

        if (!line.isEmpty())
            parseMessage(line);
    }
}

void SmartLinkClient::onPingTimer()
{
    if (m_serverConnected)
        m_socket.write("ping from client\n");
}

// ── Message Parsing ──────────────────────────────────────────────────────────

void SmartLinkClient::parseMessage(const QString& msg)
{
    // Redact any token= values from log output
    if (msg.contains("token="))
        qCDebug(lcSmartLink) << "SmartLink RX:" << msg.left(msg.indexOf("token=") + 6) + "***REDACTED***";
    else
        qCDebug(lcSmartLink) << "SmartLink RX:" << msg.left(120);

    if (msg.startsWith("radio list ")) {
        parseRadioList(msg);
    } else if (msg.startsWith("radio connect_ready")) {
        parseConnectReady(msg);
    } else if (msg.startsWith("application info")) {
        parseApplicationInfo(msg);
    } else if (msg.startsWith("application user_settings")) {
        parseUserSettings(msg);
    } else if (msg.startsWith("application registration_invalid")) {
        qCWarning(lcSmartLink) << "SmartLinkClient: registration invalid — token rejected";
        m_pingTimer.stop();
        m_serverConnected = false;
        m_authenticated = false;
        m_idToken.clear();
        if (m_socket.state() != QAbstractSocket::UnconnectedState) {
            m_socket.disconnectFromHost();
        }
        emit authFailed("SmartLink registration invalid — please re-login");
    } else if (msg.startsWith("radio test_connection")) {
        parseTestResults(msg);
    } else if (!msg.startsWith("ping")) {
        qCDebug(lcSmartLink) << "SmartLinkClient: unhandled message:" << msg.left(80);
    }
}

void SmartLinkClient::parseRadioList(const QString& msg)
{
    // "radio list name=<> callsign=<> serial=<>|name=<> ...|..."
    const QString body = msg.mid(11); // skip "radio list "
    const QStringList radioMsgs = body.split('|', Qt::SkipEmptyParts);

    QList<WanRadioInfo> radios;

    for (const auto& radioMsg : radioMsgs) {
        QMap<QString, QString> kv;
        // Parse key=value pairs (space-separated, values don't contain spaces)
        const QStringList words = radioMsg.split(' ', Qt::SkipEmptyParts);
        for (const auto& w : words) {
            int eq = w.indexOf('=');
            if (eq > 0)
                kv[w.left(eq)] = w.mid(eq + 1);
        }

        WanRadioInfo info;
        info.serial            = kv.value("serial");
        info.nickname          = kv.value("radio_name", kv.value("name"));
        info.callsign          = kv.value("callsign");
        info.model             = kv.value("model");
        info.status            = kv.value("status", "Unknown");
        info.publicIp          = kv.value("public_ip");
        int manualTls          = kv.value("public_tls_port", "-1").toInt();
        int manualUdp          = kv.value("public_udp_port", "-1").toInt();
        info.upnpSupported     = kv.value("upnp_supported") == "1";
        int upnpTls            = kv.value("public_upnp_tls_port", "-1").toInt();
        int upnpUdp            = kv.value("public_upnp_udp_port", "-1").toInt();
        info.licensedClients   = kv.value("licensed_clients", "1").toInt();
        info.maxLicensedVersion = kv.value("max_licensed_version", "v1");
        info.guiClientPrograms = kv.value("gui_client_programs");
        info.guiClientStations = kv.value("gui_client_stations");
        info.guiClientHandles  = kv.value("gui_client_handles");
        info.guiClientIps      = kv.value("gui_client_ips");
        info.guiClientHosts    = kv.value("gui_client_hosts");

        // Port selection: prefer manual port forwards, fall back to UPnP
        // (matches FlexLib WanServer.cs logic)
        if (manualTls > 0 && manualUdp > 0) {
            info.publicTlsPort = manualTls;
            info.publicUdpPort = manualUdp;
        } else if (info.upnpSupported && upnpTls > 0 && upnpUdp > 0) {
            info.publicTlsPort = upnpTls;
            info.publicUdpPort = upnpUdp;
        } else {
            info.publicTlsPort = -1;
            info.publicUdpPort = -1;
        }

        // Determine if hole punch is needed
        bool hasForwardedPorts = info.publicTlsPort > 0 && info.publicUdpPort > 0;
        info.requiresHolePunch = !info.upnpSupported && !hasForwardedPorts;

        if (!info.serial.isEmpty())
            radios.append(info);
    }

    qCDebug(lcSmartLink) << "SmartLinkClient: received" << radios.size() << "WAN radio(s)";
    for (const auto& r : radios)
        qCDebug(lcSmartLink) << "  " << r.model << r.nickname << r.serial << r.status
                 << "ip:" << r.publicIp << "tls:" << r.publicTlsPort
                 << "udp:" << r.publicUdpPort << "upnp:" << r.upnpSupported
                 << "holePunch:" << r.requiresHolePunch;

    emit radioListReceived(radios);
}

void SmartLinkClient::parseConnectReady(const QString& msg)
{
    // "radio connect_ready handle=<h> serial=<s>"
    QMap<QString, QString> kv;
    const QStringList words = msg.split(' ', Qt::SkipEmptyParts);
    for (const auto& w : words) {
        int eq = w.indexOf('=');
        if (eq > 0)
            kv[w.left(eq)] = w.mid(eq + 1);
    }

    QString handle = kv.value("handle");
    QString serial = kv.value("serial");

    qCDebug(lcSmartLink) << "SmartLinkClient: connect ready, serial:" << serial;
    emit connectReady(handle, serial);
}

void SmartLinkClient::parseApplicationInfo(const QString& msg)
{
    const QStringList words = msg.split(' ', Qt::SkipEmptyParts);
    for (const auto& w : words) {
        if (w.startsWith("public_ip="))
            m_publicIp = w.mid(10);
    }
    qCDebug(lcSmartLink) << "SmartLinkClient: public IP:" << m_publicIp;
}

void SmartLinkClient::parseUserSettings(const QString& msg)
{
    const QStringList words = msg.split(' ', Qt::SkipEmptyParts);
    for (const auto& w : words) {
        int eq = w.indexOf('=');
        if (eq <= 0) continue;
        QString key = w.left(eq);
        QString val = w.mid(eq + 1);
        if (key == "callsign")   m_userCallsign = val;
        else if (key == "first_name") m_userFirstName = val;
        else if (key == "last_name")  m_userLastName = val;
    }
    qCDebug(lcSmartLink) << "SmartLinkClient: user:" << m_userFirstName << m_userLastName
             << "callsign:" << m_userCallsign;
}

void SmartLinkClient::parseTestResults(const QString& msg)
{
    QMap<QString, QString> kv;
    const QStringList words = msg.split(' ', Qt::SkipEmptyParts);
    for (const auto& w : words) {
        int eq = w.indexOf('=');
        if (eq > 0)
            kv[w.left(eq)] = w.mid(eq + 1);
    }

    qCDebug(lcSmartLink) << "SmartLinkClient: test_connection results for" << kv.value("serial")
             << "upnpTcp:" << kv.value("upnp_tcp_port_working")
             << "upnpUdp:" << kv.value("upnp_udp_port_working")
             << "fwdTcp:" << kv.value("forward_tcp_port_working")
             << "fwdUdp:" << kv.value("forward_udp_port_working")
             << "holePunch:" << kv.value("nat_supports_hole_punch");

    emit testConnectionResult(
        kv.value("serial"),
        kv.value("upnp_tcp_port_working") == "true",
        kv.value("upnp_udp_port_working") == "true",
        kv.value("forward_tcp_port_working") == "true",
        kv.value("forward_udp_port_working") == "true",
        kv.value("nat_supports_hole_punch") == "true"
    );
}

} // namespace AetherSDR
