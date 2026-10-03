#include "PeripheralRemovalGuard.h"
#include "PeripheralSettings.h"
#include "PgxlConnection.h"
#include "PeripheralAuthCode.h"
#include "LogManager.h"

namespace AetherSDR {

PgxlConnection::PgxlConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &PgxlConnection::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &PgxlConnection::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &PgxlConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &PgxlConnection::onError);

    // Starts at the receive rate; status frames move it. See the header for
    // where the two numbers come from -- both measured on this amplifier.
    m_pollTimer.setInterval(kPollRxMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &PgxlConnection::pollStatus);

    // Retries every 5s indefinitely until the device returns or the user disconnects.
    // This is intentional for a LAN peripheral that may be power-cycling.
    m_reconnectTimer.setParent(this);
    m_reconnectTimer.setObjectName(QStringLiteral("pgxlReconnectTimer"));
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected && !m_authBlocked && !m_lastHost.isEmpty() && reconnectAllowed()) {
            if (m_lastAutomatic) {
                autoConnectToPgxl(m_lastHost, m_lastPort);
            } else {
                connectToPgxl(m_lastHost, m_lastPort);
            }
        }
    });
    m_authTimer.setSingleShot(true);
    m_authTimer.setInterval(5000);
    connect(&m_authTimer, &QTimer::timeout, this, &PgxlConnection::onAuthTimeout);
}

void PgxlConnection::onAuthTimeout()
{
    failAuthentication("Authentication timed out", recordPeripheralAuthFailure(m_authFailures) >= 3);
}

void PgxlConnection::setAuthCode(const QString& code)
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

void PgxlConnection::setAuthCodeForAttempt(quint64 attempt, const QString& code,
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

bool PgxlConnection::reconnectAllowed() const
{
    return m_autoReconnect && PeripheralSettings::autoConnect(QStringLiteral("pgxl"));
}

void PgxlConnection::connectToPgxl(const QString& host, quint16 port)
{
    const bool wasConnected = m_connected;
    beginAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void PgxlConnection::autoConnectToPgxl(const QString& host, quint16 port)
{
    const bool wasConnected = m_connected;
    beginAutomaticAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void PgxlConnection::tryAlternatePgxl(const QString& host, quint16 port)
{
    const bool wasConnected = m_connected;
    beginAlternateAttemptAt(host, port);
    openSocket(host, port, wasConnected);
}

void PgxlConnection::openSocket(const QString& host, quint16 port, bool wasConnected)
{
    // Every path that opens a socket (explicit, automatic, alternate, reconnect)
    // lands here; a pending removal must stop all of them.
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::Pgxl)) {
        return;
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_deliberateDisconnect = true;
        m_socket.abort();  // disconnected may be emitted synchronously
        m_deliberateDisconnect = false;
    }
    if (wasConnected) {
        emit disconnected();
    }
    qCDebug(lcTuner) << "PgxlConnection: connecting to" << host << ":" << port;
    if (m_connectTransport) {
        m_connectTransport(host, port);
    } else {
        m_socket.connectToHost(host, port);
    }
}

void PgxlConnection::beginAutomaticAttemptAt(const QString& host, quint16 port)
{
    beginAttemptAt(host, port);
    m_attemptAutomatic = true;
    m_lastAutomatic = true;
}

void PgxlConnection::beginAlternateAttemptAt(const QString& host, quint16 port)
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

void PgxlConnection::beginAttemptAt(const QString& host, quint16 port)
{
    m_attemptHost = host.trimmed();
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

void PgxlConnection::beginAttempt()
{
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
    m_seq = 0;
    m_setupReadSeq = 0;
    m_gotVersion = false;
    m_version.clear();
    m_readBuf.clear();
}

void PgxlConnection::disconnect()
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

void PgxlConnection::onConnected()
{
    m_tcpReached = true;
    qCDebug(lcTuner) << "PgxlConnection: TCP connected, waiting for version line";
}

void PgxlConnection::onDisconnected()
{
    qCDebug(lcTuner) << "PgxlConnection: disconnected";
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

void PgxlConnection::onError(QAbstractSocket::SocketError error)
{
    qCWarning(lcTuner) << "PgxlConnection: socket error" << error
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

void PgxlConnection::onReadyRead()
{
    processBytes(m_socket.readAll());
}

void PgxlConnection::processBytes(const QByteArray& bytes)
{
    m_readBuf.append(bytes);

    while (true) {
        int idx = m_readBuf.indexOf('\n');
        if (idx < 0) break;
        if (idx > kMaxPeripheralLineLength) {
            m_readBuf.clear();
            failAuthentication("PGXL protocol line exceeded limit", !m_connected);
            return;
        }

        QString line = QString::fromUtf8(m_readBuf.left(idx)).trimmed();
        m_readBuf.remove(0, idx + 1);

        if (!line.isEmpty())
            processLine(line);
    }
    if (m_readBuf.size() > kMaxPeripheralLineLength) {
        m_readBuf.clear();
        failAuthentication("PGXL protocol line exceeded limit", !m_connected);
    }
}

void PgxlConnection::processLine(const QString& line)
{
    // Version line: V3.8.9
    if (!m_gotVersion && line.startsWith('V')) {
        const bool requiresAuth = line.endsWith(" AUTH");
        m_version = requiresAuth ? line.mid(1, line.size() - 6) : line.mid(1);
        m_gotVersion = true;
        qCInfo(lcTuner) << "PgxlConnection: PGXL version" << m_version;
        if (requiresAuth) {
            qCInfo(lcTuner) << "PgxlConnection: authorization challenge received";
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
            if (line == QStringLiteral("R1|0|Authorized")) {
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

    // Alert: M|<text>, or M| to clear. Its own frame type — the same one the
    // tuner sends, on the same vendor's protocol. Unlike R and S it carries
    // no sequence number, because there is no command to correlate it with:
    // it is broadcast to every connected client rather than answering the one
    // that acted. An empty body is the clear, not an alert whose text happens
    // to be blank.
    if (line.startsWith('M') && line.size() > 1 && line[1] == '|') {
        const QString text = line.mid(2).trimmed();
        qCDebug(lcTuner) << "PgxlConnection: alert" << (text.isEmpty() ? "(cleared)" : text);
        emit alertChanged(text);
        return;
    }

    // Response: R<seq>|<code>|<body>
    if (line.startsWith('R')) {
        int pipe1 = line.indexOf('|');
        int pipe2 = (pipe1 >= 0) ? line.indexOf('|', pipe1 + 1) : -1;
        if (pipe2 >= 0) {
            const quint32 seq = line.mid(1, pipe1 - 1).toUInt();
            const QString code = line.mid(pipe1 + 1, pipe2 - pipe1 - 1).trimmed();
            QString body = line.mid(pipe2 + 1).trimmed();

            // A refusal. The amplifier answers a bad parameter with 50000013
            // and an unknown command with 50000015, both carrying an EMPTY
            // body — so a reply that is only an error code reads as "nothing
            // to parse" unless the code itself is looked at. Not looking is
            // how a `setup` write that changed nothing went unnoticed through
            // a whole round of testing.
            if (!code.isEmpty() && code != QLatin1String("0")) {
                qCWarning(lcTuner)
                    << "PgxlConnection: command" << seq << "refused, code" << code;
                // Release an outstanding `setup read`. Left armed it would
                // never be answered, canWriteSetup() would stay false forever,
                // and both MEffA and fan mode would be silently inert for the
                // life of the connection.
                if (m_setupReadSeq != 0 && seq == m_setupReadSeq)
                    m_setupReadSeq = 0;
                emit commandRefused(seq, code);
                return;
            }

            if (!body.isEmpty()) {
                QMap<QString, QString> kvs;
                const auto parts = body.split(' ', Qt::SkipEmptyParts);
                for (const auto& part : parts) {
                    int eq = part.indexOf('=');
                    if (eq > 0)
                        kvs.insert(part.left(eq), part.mid(eq + 1));
                }
                if (!kvs.isEmpty()) {
                    // A `setup read` reply looks exactly like a status reply —
                    // key/value pairs in an R frame — so it is told apart by
                    // the sequence number that asked for it, not by shape.
                    if (m_setupReadSeq != 0 && seq == m_setupReadSeq) {
                        m_setupReadSeq = 0;
                        emit setupRead(kvs);
                    } else {
                        m_pollInFlight = false;   // reply landed
                    applyPollRateFor(kvs);
                        emit statusUpdated(kvs);
                    }
                }
            }
        }
        return;
    }

    // Status push: S0|... (PGXL may push unsolicited status)
    if (line.startsWith('S')) {
        int pipe = line.indexOf('|');
        if (pipe < 0) return;

        QString rest = line.mid(pipe + 1);
        int firstEq = rest.indexOf('=');
        if (firstEq < 0) return;
        // PGXL S-push format varies by firmware:
        //   "S0|TRANSMIT_A id=39 vac=241 ..."  — prefix word before first key
        //   "S0|id=39 vac=241 ..."              — KV pairs start immediately
        // When there is no space before the first '=' the body is all KV pairs;
        // fall through to parse it directly instead of returning early.
        int lastSpaceBeforeEq = rest.lastIndexOf(' ', firstEq);
        QString kvString = (lastSpaceBeforeEq >= 0)
                               ? rest.mid(lastSpaceBeforeEq + 1)
                               : rest;
        QMap<QString, QString> kvs;
        const auto parts = kvString.split(' ', Qt::SkipEmptyParts);
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
        return;
    }
}

void PgxlConnection::finishHandshake()
{
    m_authBlocked = false;
    m_authFailures = 0;
    sendCommand("info");
    // Setup writes carry the whole group; read it before controls are enabled.
    m_setupReadSeq = sendCommand("setup read");
    sendCommand("status");
    m_connected = true;
    m_pollTimer.start();
    emit connected();
}

void PgxlConnection::sendAuthentication()
{
    if (m_authCode.isEmpty()) {
        failAuthentication("Authorization code required");
        return;
    }
    if (!validPeripheralAuthCode(m_authCode)) {
        failAuthentication("Invalid authorization code");
        return;
    }
    // Firmware 3.9.1 expects code= and answers R1|0|Authorized. Avoid the
    // command logger, which records its entire argument.
    m_authPending = true;
    m_socket.write(peripheralAuthCommand(PeripheralAuthProtocol::Pgxl, m_authCode));
    m_seq = 1;
    m_authTimer.start();
}

void PgxlConnection::failAuthentication(const QString& reason, bool blockReconnect)
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

// TRANSMIT_A / TRANSMIT_B are the amplifier's keyed states; IDLE, STANDBY and
// POWERUP are not. Anything unrecognised is treated as not transmitting, so a
// new state string cannot pin the poll rate high forever.
void PgxlConnection::applyPollRateFor(const QMap<QString, QString>& kvs)
{
    if (!kvs.contains(QStringLiteral("state"))) return;
    const QString st = kvs.value(QStringLiteral("state"));
    setTransmitting(st == QLatin1String("TRANSMIT_A")
                    || st == QLatin1String("TRANSMIT_B"));
}

void PgxlConnection::setTransmitting(bool tx)
{
    if (m_transmitting == tx) return;
    m_transmitting = tx;
    const int interval = tx ? kPollTxMs : kPollRxMs;
    if (m_pollTimer.interval() != interval) {
        m_pollTimer.setInterval(interval);
        // Restart so the new rate applies now rather than after the remainder
        // of a 250 ms receive tick.
        if (m_pollTimer.isActive()) m_pollTimer.start();
    }
}

quint32 PgxlConnection::sendCommand(const QString& cmd)
{
    quint32 seq = ++m_seq;
    QString line = QString("C%1|%2\n").arg(seq).arg(cmd);
    if (m_commandWriter) {
        m_commandWriter(line.toUtf8());
    } else {
        m_socket.write(line.toUtf8());
    }
    // Setup writes carry the device's authcode even when the operator only
    // changed fan mode or MEffA. Never copy that credential into support logs.
    qCDebug(lcTuner) << "PgxlConnection: sent"
                     << (cmd.contains(QStringLiteral("authcode="), Qt::CaseInsensitive)
                             ? QStringLiteral("<setup command redacted>")
                             : line.trimmed());
    return seq;
}

void PgxlConnection::pollStatus()
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
