#include "PeripheralRemovalGuard.h"
#include "PeripheralSettings.h"
#include "TgxlConnection.h"
#include "PeripheralAuthCode.h"
#include "LogManager.h"

namespace AetherSDR {

TgxlConnection::TgxlConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &TgxlConnection::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &TgxlConnection::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &TgxlConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &TgxlConnection::onError);

    // Starts at the receive rate; status frames move it. See the header
    // for where these two numbers come from -- both are measured, not
    // chosen for comfort.
    m_pollTimer.setInterval(kPollRxMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &TgxlConnection::pollStatus);

    // Retries every 5s indefinitely until the device returns or the user disconnects.
    // This is intentional for a LAN peripheral that may be power-cycling.
    m_reconnectTimer.setParent(this);
    m_reconnectTimer.setObjectName(QStringLiteral("tgxlReconnectTimer"));
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected && !m_authBlocked && !m_lastHost.isEmpty() && reconnectAllowed()) {
            if (m_lastAutomatic) {
                autoConnectToTgxl(m_lastHost, m_lastPort);
            } else {
                connectToTgxl(m_lastHost, m_lastPort);
            }
        }
    });
    m_authTimer.setSingleShot(true);
    m_authTimer.setInterval(5000);
    connect(&m_authTimer, &QTimer::timeout, this, &TgxlConnection::onAuthTimeout);
}

void TgxlConnection::onAuthTimeout()
{
    failAuthentication("Authentication timed out", recordPeripheralAuthFailure(m_authFailures) >= 3);
}

void TgxlConnection::setAuthCode(const QString& code)
{
    const bool wasBlocked = m_authBlocked;
    m_authCode = code;
    m_userAuthCode = !code.isEmpty();
    m_userAuthEndpoint = m_userAuthCode
        ? m_lastHost.trimmed().toLower() + QLatin1Char('|') + QString::number(m_lastPort)
        : QString();
    m_authBlocked = false;
    m_authFailures = 0;
    if (wasBlocked) {
        emit authBlockCleared();
    }
    if (code.isEmpty() && m_waitingForAuthCode) {
        failAuthentication("Authorization code required");
    }
}

void TgxlConnection::setAuthCodeForAttempt(quint64 attempt, const QString& code,
                                            bool credentialStoreUnavailable)
{
    if (m_waitingForAuthCode && attempt == m_authAttempt) {
        if (code.isEmpty()) {
            // A keychain outage says nothing about the saved code, so
            // auto-reconnect stays available once the keychain returns. An
            // alternate address has no saved code of its own; reconnects to
            // the target keep theirs.
            failAuthentication(credentialStoreUnavailable
                ? "Stored authorization code unavailable" : "Authorization code required",
                !credentialStoreUnavailable && !m_alternateAttempt);
            return;
        }
        // Restoring the same saved code on a reconnect must not replenish the
        // failure budget. Only an operator-entered code or success resets it.
        m_authCode = code;
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
        m_waitingForAuthCode = false;
        sendAuthentication();
    }
}

bool TgxlConnection::reconnectAllowed() const
{
    return m_autoReconnect && PeripheralSettings::autoConnect(QStringLiteral("tgxl"));
}

void TgxlConnection::connectToTgxl(const QString& host, quint16 port)
{
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::Tgxl)) {
        return;
    }
    const bool wasConnected = m_connected;
    beginAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void TgxlConnection::autoConnectToTgxl(const QString& host, quint16 port)
{
    // Checked before any attempt state changes, so a blocked attempt leaves
    // the retry target and timer alone.
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::Tgxl)) {
        return;
    }
    const bool wasConnected = m_connected;
    beginAutomaticAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void TgxlConnection::tryAlternateTgxl(const QString& host, quint16 port)
{
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::Tgxl)) {
        return;
    }
    const bool wasConnected = m_connected;
    beginAlternateAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void TgxlConnection::openSocket(const QString& host, quint16 port, bool wasConnected)
{
    // Every path that opens a socket (explicit, automatic, alternate, reconnect)
    // lands here; a pending removal must stop all of them.
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::Tgxl)) {
        return;
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_deliberateDisconnect = true;
        m_socket.abort();
        m_deliberateDisconnect = false;
    }
    if (wasConnected) {
        emit disconnected();
    }
    qCDebug(lcTuner) << "TgxlConnection: connecting to" << host << ":" << port;
    if (m_connectTransport) {
        m_connectTransport(host, port);
    } else {
        m_socket.connectToHost(host, port);
    }
}

void TgxlConnection::beginAutomaticAttemptAt(const QString& host, quint16 port)
{
    beginAttemptAt(host, port);
    m_attemptAutomatic = true;
    m_lastAutomatic = true;
}

void TgxlConnection::beginAlternateAttemptAt(const QString& host, quint16 port)
{
    const QString target = m_lastHost;
    const quint16 targetPort = m_lastPort;
    const bool targetAutomatic = m_lastAutomatic;
    beginAttemptAt(host, port);
    m_lastHost = target;
    m_lastPort = targetPort;
    m_lastAutomatic = targetAutomatic;
    m_alternateAttempt = true;
}

void TgxlConnection::beginAttemptAt(const QString& host, quint16 port)
{
    m_attemptHost = host.trimmed();
    m_attemptPort = port;
    const QString target = host.trimmed().toLower() + QLatin1Char('|') + QString::number(port);
    if (m_userAuthCode && m_userAuthEndpoint != target) {
        m_authCode.clear();
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
        emit enteredAuthCodeDiscarded();
    }
    m_lastHost = host;
    m_lastPort = port;
    m_lastAutomatic = false;
    beginAttempt();
}

void TgxlConnection::beginAttempt()
{
    // A code verified on a previous socket must be reloaded only after the
    // next peer is known; its saved endpoint is checked by the UI vault.
    if (!m_userAuthCode) {
        m_authCode.clear();
    }
    ++m_authAttempt;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    m_authTimer.stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    m_authCloseReported = false;
    m_tcpReached = false;
    m_attemptAutomatic = false;
    m_alternateAttempt = false;
    m_pollTimer.stop();
    m_pollInFlight = false;
    m_connected = false;
    m_gotVersion = false;
    m_version.clear();
    m_readBuf.clear();
    m_seq = 0;
}

void TgxlConnection::disconnect()
{
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_authTimer.stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    const bool discardedCode = m_userAuthCode;
    if (discardedCode) {
        m_authCode.clear();
    }
    m_userAuthCode = false;
    m_userAuthEndpoint.clear();
    if (discardedCode) {
        emit enteredAuthCodeDiscarded();
    }
    m_pollTimer.stop();
    m_pollInFlight = false;
    m_connected = false;
    m_socket.disconnectFromHost();
    if (wasConnected) {
        emit disconnected();
    }
}

void TgxlConnection::onConnected()
{
    m_tcpReached = true;
    qCDebug(lcTuner) << "TgxlConnection: TCP connected, waiting for version line";
    // TGXL sends V<version>\n first, then we send our init commands
}

void TgxlConnection::onDisconnected()
{
    qCDebug(lcTuner) << "TgxlConnection: disconnected";
    m_pollTimer.stop();
    const bool rejectedDuringAuth = m_authPending;
    m_authTimer.stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    if (rejectedDuringAuth && !m_deliberateDisconnect) {
        m_authBlocked = recordPeripheralAuthFailure(m_authFailures) >= 3;
        if (m_authBlocked) {
            m_authCode.clear();
            m_userAuthCode = false;
            m_userAuthEndpoint.clear();
        }
        m_authCloseReported = true;
        emit connectionFailed("Connection closed during authentication");
    }
    m_pollInFlight = false;
    const bool wasConnected = m_connected;
    m_connected = false;
    if (wasConnected) {
        emit disconnected();
    }
    if (!m_deliberateDisconnect && !m_authBlocked && reconnectAllowed() && !m_lastHost.isEmpty()) {
        m_reconnectTimer.start();
    }
    m_deliberateDisconnect = false;
}

void TgxlConnection::onError(QAbstractSocket::SocketError error)
{
    qCWarning(lcTuner) << "TgxlConnection: socket error" << error
                        << m_socket.errorString();
    if (!m_authPending && !m_authCloseReported) {
        emit connectionFailed(m_socket.errorString());
    }
    if (m_attemptAutomatic && !m_tcpReached && !m_deliberateDisconnect && !m_authBlocked) {
        emit unreachable(m_attemptHost);
    }
    // A failed reconnect attempt arrives here (not via onDisconnected) because
    // the socket never reached ConnectedState. Re-arm so we keep retrying until
    // the device returns or the user disconnects. isActive() prevents double-arm
    // when a live drop emits both errorOccurred and disconnected.
    if (!m_deliberateDisconnect && !m_authBlocked && !m_authPending && reconnectAllowed() && !m_connected
            && !m_lastHost.isEmpty() && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void TgxlConnection::onReadyRead()
{
    processBytes(m_socket.readAll());
}

void TgxlConnection::processBytes(const QByteArray& bytes)
{
    m_readBuf.append(bytes);

    while (true) {
        int idx = m_readBuf.indexOf('\n');
        if (idx < 0) break;
        if (idx > kMaxPeripheralLineLength) {
            m_readBuf.clear();
            failAuthentication("TGXL protocol line exceeded limit", !m_connected);
            return;
        }

        QString line = QString::fromUtf8(m_readBuf.left(idx)).trimmed();
        m_readBuf.remove(0, idx + 1);

        if (!line.isEmpty())
            processLine(line);
    }
    if (m_readBuf.size() > kMaxPeripheralLineLength) {
        m_readBuf.clear();
        failAuthentication("TGXL protocol line exceeded limit", !m_connected);
    }
}

void TgxlConnection::processLine(const QString& line)
{
    // Version line: V1.2.17
    if (!m_gotVersion && line.startsWith('V')) {
        const bool requiresAuth = line.endsWith(" AUTH");
        m_version = requiresAuth ? line.mid(1, line.size() - 6) : line.mid(1);
        m_gotVersion = true;
        qCDebug(lcTuner) << "TgxlConnection: TGXL version" << m_version;
        if (requiresAuth) {
            qCInfo(lcTuner) << "TgxlConnection: authorization challenge received";
            if (m_authCode.isEmpty()) {
                m_waitingForAuthCode = true;
                emit authCodeRequired(m_authAttempt);
            } else {
                sendAuthentication();
            }
        } else {
            // A LAN greeting did not verify a newly entered code. Discard it
            // rather than carrying it to a later, possibly different host.
            if (m_userAuthCode) {
                m_authCode.clear();
                m_userAuthCode = false;
                m_userAuthEndpoint.clear();
            }
            finishHandshake();
        }
        return;
    }

    if (m_waitingForAuthCode) {
        return;
    }

    if (m_authPending) {
        if (line.startsWith('R')) {
            // The 4O3A TGXL API documents R1|0|auth OK and R1|0|Unauthorized;
            // firmware 1.2.17 was observed returning R0|0|auth OK. The numeric
            // result alone does not distinguish success.
            if (line == QStringLiteral("R0|0|auth OK")
                || line == QStringLiteral("R1|0|auth OK")) {
                m_authTimer.stop();
                m_authPending = false;
                const QString acceptedCode = m_userAuthCode ? m_authCode : QString();
                m_authCode.clear();
                if (m_userAuthCode) {
                    m_userAuthCode = false;
                    m_userAuthEndpoint.clear();
                    emit authCodeAccepted(acceptedCode);
                }
                finishHandshake();
            } else {
                failAuthentication("Authorization code rejected");
            }
        }
        return;
    }

    // Alert: M|<text>, or M| to clear. The tuner pushes these to every
    // client when a tune cannot proceed — "LOW RF POWER" is the one the
    // capture caught, raised ~40 ms after a tune starts with insufficient
    // drive and cleared unprompted ~3 s later. Checked before the R and S
    // branches: 'M' is its own frame type, not a response.
    if (line.startsWith('M') && line.size() > 1 && line[1] == '|') {
        const QString text = line.mid(2).trimmed();
        qCDebug(lcTuner) << "TgxlConnection: alert" << (text.isEmpty() ? "(cleared)" : text);
        emit alertChanged(text);
        return;
    }

    // Response: R<seq>|<code>|<body>
    // Status poll responses contain fwd/swr meter data as KV pairs.
    // Format: R<seq>|0|key=val key=val ...
    if (line.startsWith('R')) {
        // Extract body after second pipe: R<seq>|<code>|<body>
        int pipe1 = line.indexOf('|');
        int pipe2 = (pipe1 >= 0) ? line.indexOf('|', pipe1 + 1) : -1;
        if (pipe2 >= 0) {
            QString body = line.mid(pipe2 + 1).trimmed();
            if (!body.isEmpty()) {
                QMap<QString, QString> kvs;
                const auto parts = body.split(' ', Qt::SkipEmptyParts);
                for (const auto& part : parts) {
                    int eq = part.indexOf('=');
                    if (eq > 0)
                        kvs.insert(part.left(eq), part.mid(eq + 1));
                }
                if (!kvs.isEmpty()) {
                    m_pollInFlight = false;   // reply landed
                    applyPollRateFor(kvs);
                    emit statusUpdated(kvs);
                }
            }
        }
        return;
    }

    // State push: S0|state key=val key=val ...
    // Status poll response: S<seq>|status key=val key=val ...
    if (line.startsWith('S')) {
        int pipe = line.indexOf('|');
        if (pipe < 0) return;

        QString rest = line.mid(pipe + 1);

        // Find object name — everything before first key=val
        // "state bypassA=0 ..." or "status fwd=0.0000 ..."
        int firstEq = rest.indexOf('=');
        if (firstEq < 0) return;
        int lastSpaceBeforeEq = rest.lastIndexOf(' ', firstEq);
        if (lastSpaceBeforeEq < 0) return;

        QString object = rest.left(lastSpaceBeforeEq).trimmed();
        QString kvString = rest.mid(lastSpaceBeforeEq + 1);

        // Parse key=value pairs
        QMap<QString, QString> kvs;
        const auto parts = kvString.split(' ', Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            int eq = part.indexOf('=');
            if (eq > 0)
                kvs.insert(part.left(eq), part.mid(eq + 1));
        }

        if (object == "state") {
            emit stateUpdated(kvs);
        } else if (object == "status") {
            m_pollInFlight = false;   // reply landed
                    applyPollRateFor(kvs);
            emit statusUpdated(kvs);
        }
        return;
    }
}

void TgxlConnection::finishHandshake()
{
    m_authBlocked = false;
    m_authFailures = 0;
    sendCommand("info");
    sendCommand("status");
    m_connected = true;
    m_pollTimer.start();
    emit connected();
}

void TgxlConnection::sendAuthentication()
{
    if (m_authCode.isEmpty()) {
        failAuthentication("Authorization code required");
        return;
    }
    if (!validPeripheralAuthCode(m_authCode)) {
        failAuthentication("Invalid authorization code");
        return;
    }
    // Firmware 1.2.17 replies R0|0|auth OK to C1. Write directly so the
    // credential never passes through sendCommand's debug log.
    m_authPending = true;
    m_socket.write(peripheralAuthCommand(PeripheralAuthProtocol::Tgxl, m_authCode));
    m_seq = 1;
    m_authTimer.start();
}

void TgxlConnection::failAuthentication(const QString& reason, bool blockReconnect)
{
    m_authTimer.stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    m_authBlocked = blockReconnect;
    if (blockReconnect) {
        m_authCode.clear();
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
    }
    m_reconnectTimer.stop();
    m_readBuf.clear();
    emit connectionFailed(reason);
    m_socket.abort();
    if (!m_authBlocked && reconnectAllowed() && !m_lastHost.isEmpty()
        && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

// Either port keyed counts: the tuner is passing power on one of them, and
// that is what the meter is reading.
void TgxlConnection::applyPollRateFor(const QMap<QString, QString>& kvs)
{
    if (!kvs.contains("pttA") && !kvs.contains("pttB")) return;
    const bool tx = kvs.value("pttA").toInt() != 0 || kvs.value("pttB").toInt() != 0;
    setTransmitting(tx);
}

void TgxlConnection::setTransmitting(bool tx)
{
    if (m_transmitting == tx) return;
    m_transmitting = tx;
    const int interval = tx ? kPollTxMs : kPollRxMs;
    if (m_pollTimer.interval() != interval) {
        m_pollTimer.setInterval(interval);
        // Restart so the new rate takes effect now rather than after the
        // remainder of a 250 ms receive tick -- which is most of the first
        // syllable.
        if (m_pollTimer.isActive()) m_pollTimer.start();
    }
    qCDebug(lcTuner) << "TgxlConnection: poll rate ->" << interval << "ms (tx" << tx << ")";
}

quint32 TgxlConnection::sendCommand(const QString& cmd)
{
    quint32 seq = ++m_seq;
    QString line = QString("C%1|%2\n").arg(seq).arg(cmd);
    m_socket.write(line.toUtf8());
    qCDebug(lcTuner) << "TgxlConnection: sent" << line.trimmed();
    return seq;
}

void TgxlConnection::adjustRelay(int relay, int direction)
{
    if (!m_connected) return;
    if (relay < 0 || relay > 2) return;
    int move = (direction > 0) ? 1 : -1;
    sendCommand(QString("tune relay=%1 move=%2").arg(relay).arg(move));
}

void TgxlConnection::requestAutotune()
{
    if (!m_connected) return;
    sendCommand("autotune");
}

void TgxlConnection::pollStatus()
{
    if (!m_connected) return;
    if (m_pollInFlight && m_pollSent.isValid()
        && m_pollSent.elapsed() < kPollStaleMs) {
        return;   // previous poll still outstanding
    }
    m_pollInFlight = true;
    m_pollSent.restart();
    sendCommand("status");
}

} // namespace AetherSDR
