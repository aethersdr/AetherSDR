#include "PgxlConnection.h"
#include "LogManager.h"

namespace AetherSDR {

PgxlConnection::PgxlConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &PgxlConnection::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &PgxlConnection::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &PgxlConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &PgxlConnection::onError);

    m_pollTimer.setInterval(200);  // 5 Hz for responsive metering
    connect(&m_pollTimer, &QTimer::timeout, this, &PgxlConnection::pollStatus);

    // Retries every 5s indefinitely until the device returns or the user disconnects.
    // This is intentional for a LAN peripheral that may be power-cycling.
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected && !m_lastHost.isEmpty()) {
            connectToPgxl(m_lastHost, m_lastPort);
        }
    });
}

void PgxlConnection::connectToPgxl(const QString& host, quint16 port)
{
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    if (m_connected) {
        m_deliberateDisconnect = true;
        m_pollTimer.stop();
        m_connected = false;
        m_socket.abort();  // synchronous — onDisconnected will not fire
        m_deliberateDisconnect = false;
    }
    m_seq = 0;
    m_setupReadSeq = 0;
    m_gotVersion = false;
    m_version.clear();
    m_readBuf.clear();
    qCDebug(lcTuner) << "PgxlConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
}

void PgxlConnection::disconnect()
{
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_connected = false;
    m_socket.disconnectFromHost();
}

void PgxlConnection::onConnected()
{
    qCDebug(lcTuner) << "PgxlConnection: TCP connected, waiting for version line";
}

void PgxlConnection::onDisconnected()
{
    qCDebug(lcTuner) << "PgxlConnection: disconnected";
    m_pollTimer.stop();
    m_connected = false;
    emit disconnected();
    if (!m_deliberateDisconnect && m_autoReconnect && !m_lastHost.isEmpty()) {
        m_reconnectTimer.start();
    }
    m_deliberateDisconnect = false;
}

void PgxlConnection::onError(QAbstractSocket::SocketError error)
{
    qCWarning(lcTuner) << "PgxlConnection: socket error" << error
                        << m_socket.errorString();
    // A failed reconnect attempt arrives here (not via onDisconnected) because
    // the socket never reached ConnectedState. Re-arm so we keep retrying until
    // the device returns or the user disconnects. isActive() prevents double-arm
    // when a live drop emits both errorOccurred and disconnected.
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected
            && !m_lastHost.isEmpty() && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void PgxlConnection::onReadyRead()
{
    m_readBuf.append(m_socket.readAll());

    while (true) {
        int idx = m_readBuf.indexOf('\n');
        if (idx < 0) break;

        QString line = QString::fromUtf8(m_readBuf.left(idx)).trimmed();
        m_readBuf.remove(0, idx + 1);

        if (!line.isEmpty())
            processLine(line);
    }
}

void PgxlConnection::processLine(const QString& line)
{
    // Version line: V3.8.9
    if (!m_gotVersion && line.startsWith('V')) {
        m_version = line.mid(1);
        m_gotVersion = true;
        qCInfo(lcTuner) << "PgxlConnection: PGXL version" << m_version;

        sendCommand("info");
        // Read the stored configuration up front. A `setup` write has to carry
        // the whole group, so the values we are not changing have to be known
        // before the operator can change the one they are.
        m_setupReadSeq = sendCommand("setup read");
        sendCommand("status");

        m_connected = true;
        m_pollTimer.start();
        emit connected();
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
        if (!kvs.isEmpty())
            emit statusUpdated(kvs);
        return;
    }
}

quint32 PgxlConnection::sendCommand(const QString& cmd)
{
    quint32 seq = ++m_seq;
    QString line = QString("C%1|%2\n").arg(seq).arg(cmd);
    m_socket.write(line.toUtf8());
    qCDebug(lcTuner) << "PgxlConnection: sent" << line.trimmed();
    return seq;
}

void PgxlConnection::pollStatus()
{
    if (m_connected)
        sendCommand("status");
}

} // namespace AetherSDR
