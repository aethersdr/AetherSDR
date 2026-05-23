#include "TgxlConnection.h"
#include "LogManager.h"

namespace MasterSDR {

TgxlConnection::TgxlConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &TgxlConnection::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &TgxlConnection::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &TgxlConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &TgxlConnection::onError);

    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &TgxlConnection::pollStatus);
}

void TgxlConnection::connectToTgxl(const QString& host, quint16 port)
{
    // Abort any pending or active connection before starting a new one (#1039)
    m_pollTimer.stop();
    m_connected = false;
    m_gotVersion = false;
    m_version.clear();
    m_readBuf.clear();
    m_seq = 0;
    m_socket.abort();
    qCDebug(lcTuner) << "TgxlConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
}

void TgxlConnection::disconnect()
{
    m_pollTimer.stop();
    m_connected = false;
    m_socket.disconnectFromHost();
}

void TgxlConnection::onConnected()
{
    qCDebug(lcTuner) << "TgxlConnection: TCP connected, waiting for version line";
    // TGXL sends V<version>\n first, then we send our init commands
}

void TgxlConnection::onDisconnected()
{
    qCDebug(lcTuner) << "TgxlConnection: disconnected";
    m_pollTimer.stop();
    m_connected = false;
    emit disconnected();
}

void TgxlConnection::onError(QAbstractSocket::SocketError error)
{
    qCWarning(lcTuner) << "TgxlConnection: socket error" << error
                        << m_socket.errorString();
    emit connectionFailed(m_socket.errorString());
}

void TgxlConnection::onReadyRead()
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

void TgxlConnection::processLine(const QString& line)
{
    // Version line: V1.2.17
    if (!m_gotVersion && line.startsWith('V')) {
        m_version = line.mid(1);
        m_gotVersion = true;
        qCDebug(lcTuner) << "TgxlConnection: TGXL version" << m_version;

        // Send init commands
        sendCommand("info");
        sendCommand("status");

        m_connected = true;
        m_pollTimer.start();
        emit connected();
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
                if (!kvs.isEmpty())
                    emit statusUpdated(kvs);
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
            emit statusUpdated(kvs);
        }
        return;
    }
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
    if (m_connected)
        sendCommand("status");
}

} // namespace MasterSDR
