#include "DxClusterClient.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QStringList>
#include <algorithm>

namespace AetherSDR {

DxClusterClient::DxClusterClient(QObject* parent)
    : QObject(parent)
{
    // Socket and timer are created in initialize() on the SpotClients thread (#1929).
}

void DxClusterClient::initialize()
{
    if (m_socket) return;  // already initialized

    m_socket = new QTcpSocket(this);
    m_reconnectTimer = new QTimer(this);

    connect(m_socket, &QTcpSocket::connected,    this, &DxClusterClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &DxClusterClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &DxClusterClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &DxClusterClient::onSocketError);

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &DxClusterClient::onReconnectTimer);
}

DxClusterClient::~DxClusterClient()
{
    m_intentionalDisconnect = true;
    if (m_reconnectTimer) m_reconnectTimer->stop();
    m_logFile.close();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

QString DxClusterClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/" + m_logFileName;
}

void DxClusterClient::connectToCluster(const QString& host, quint16 port, const QString& callsign)
{
    if (m_connected || m_socket->state() == QAbstractSocket::ConnectingState) {
        qCWarning(lcDxCluster) << "DxClusterClient: connect attempt already in progress";
        return;
    }

    m_host = host;
    m_port = port;
    m_callsign = callsign;
    m_loggedIn = false;
    m_intentionalDisconnect = false;
    m_readBuffer.clear();

    qCDebug(lcDxCluster) << "DxClusterClient: connecting to" << host << ":" << port;
    m_socket->connectToHost(host, port);

    // Connection timeout — capture epoch so a stale timeout from attempt N cannot
    // abort a later attempt that has already succeeded (#2380).
    const int epoch = ++m_connectEpoch;
    QTimer::singleShot(ConnectTimeoutMs, this, [this, epoch] {
        if (m_connectEpoch != epoch) return;  // superseded by a later attempt
        if (!m_connected && m_socket->state() != QAbstractSocket::ConnectedState) {
            qCWarning(lcDxCluster) << "DxClusterClient: connection timeout";
            m_socket->abort();
            emit connectionError("Connection timeout");
            scheduleReconnect();
        }
    });
}

void DxClusterClient::disconnect()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    if (m_connected) {
        m_socket->write("bye\r\n");
        m_socket->flush();
    }
    m_socket->disconnectFromHost();
}

void DxClusterClient::sendCommand(const QString& cmd)
{
    if (!m_connected) return;
    qCDebug(lcDxCluster) << "DxClusterClient TX:" << cmd;
    if (m_logFile.isOpen()) {
        m_logFile.write(("> " + cmd + "\n").toUtf8());
        m_logFile.flush();
    }
    m_socket->write((cmd + "\r\n").toLatin1());
}

// ── Socket slots ────────────────────────────────────────────────────────────

void DxClusterClient::onConnected()
{
    qCDebug(lcDxCluster) << "DxClusterClient: TCP connected to" << m_host;
    m_connected = true;
    m_reconnectAttempts = 0;

    // Open log file (truncate on each new connection)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- Connected to %1:%2 at %3 ---\n")
            .arg(m_host).arg(m_port)
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .toUtf8());
        m_logFile.flush();
    }

    emit connected();
}

void DxClusterClient::onDisconnected()
{
    qCDebug(lcDxCluster) << "DxClusterClient: disconnected";
    bool wasConnected = m_connected;
    m_connected = false;
    m_loggedIn = false;

    if (wasConnected)
        emit disconnected();

    scheduleReconnect();
}

void DxClusterClient::onSocketError(QAbstractSocket::SocketError /*err*/)
{
    QString msg = m_socket->errorString();
    qCWarning(lcDxCluster) << "DxClusterClient: socket error:" << msg;
    emit connectionError(msg);
    // When a connection attempt fails (ConnectingState → error), Qt does NOT emit
    // disconnected(), so onDisconnected() never fires. Arm the reconnect timer
    // here so the chain continues after a failed attempt (#2380).
    if (!m_connected) {
        scheduleReconnect();
    }
}

void DxClusterClient::scheduleReconnect()
{
    if (m_intentionalDisconnect) return;
    if (m_reconnectTimer->isActive()) return;  // already scheduled — don't compound backoff
    int delay = std::min(InitialReconnectDelayMs * (1 << m_reconnectAttempts),
                         MaxReconnectDelayMs);
    qCDebug(lcDxCluster) << "DxClusterClient: reconnecting in" << delay
                         << "ms (attempt" << m_reconnectAttempts + 1 << ")";
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void DxClusterClient::onReconnectTimer()
{
    if (m_intentionalDisconnect) return;
    qCDebug(lcDxCluster) << "DxClusterClient: attempting reconnect";
    connectToCluster(m_host, m_port, m_callsign);
}

// ── Line-buffered read ──────────────────────────────────────────────────────

void DxClusterClient::stripTelnetIAC()
{
    // Remove telnet IAC sequences (0xFF + command byte + option byte)
    int i = 0;
    while (i < m_readBuffer.size()) {
        if (static_cast<unsigned char>(m_readBuffer[i]) == 0xFF && i + 2 < m_readBuffer.size()) {
            m_readBuffer.remove(i, 3);
        } else {
            i++;
        }
    }
}

void DxClusterClient::onReadyRead()
{
    m_readBuffer.append(m_socket->readAll());
    stripTelnetIAC();

    while (true) {
        int idx = m_readBuffer.indexOf('\n');
        if (idx < 0) {
            // No newline yet — check for login prompt (may not end with \n)
            if (!m_loggedIn) {
                QString partial = QString::fromLatin1(m_readBuffer).trimmed();
                if (isLoginPrompt(partial)) {
                    qCDebug(lcDxCluster) << "DxClusterClient: login prompt detected (no newline):" << partial;
                    m_readBuffer.clear();
                    m_socket->write((m_callsign + "\r\n").toLatin1());
                    m_loggedIn = true;
                    qCDebug(lcDxCluster) << "DxClusterClient: sent callsign" << m_callsign;
                    sendStartupCommands();
                }
            }
            break;
        }

        QString line = QString::fromLatin1(m_readBuffer.left(idx)).trimmed();
        m_readBuffer.remove(0, idx + 1);

        // Strip non-printable control characters that some DX cluster
        // software appends to spot lines (BEL 0x07 as a TTY alert is the
        // most common, but C1 controls in 0x80–0x9F have also been seen
        // in the wild).  trimmed() above only handles ASCII whitespace,
        // so these otherwise render as missing-glyph boxes in the
        // console and contaminate spot.comment captured by the regex in
        // handleLine().  Keep TAB (0x09); drop everything else in
        // {0x00–0x1F, 0x7F–0x9F}.
        auto isControl = [](QChar c) {
            const ushort u = c.unicode();
            if (u == '\t') return false;
            return u < 0x20 || (u >= 0x7F && u <= 0x9F);
        };
        if (std::any_of(line.cbegin(), line.cend(), isControl)) {
            QString cleaned;
            cleaned.reserve(line.size());
            for (QChar c : line) {
                if (!isControl(c))
                    cleaned.append(c);
            }
            line = cleaned;
        }

        if (line.isEmpty()) continue;

        // Write to log file
        if (m_logFile.isOpen()) {
            m_logFile.write((line + "\n").toUtf8());
            m_logFile.flush();
        }

        emit rawLineReceived(line);
        handleLine(line);
    }
}

void DxClusterClient::handleLine(const QString& line)
{
    // Login prompt detection (line-based)
    if (!m_loggedIn && isLoginPrompt(line)) {
        qCDebug(lcDxCluster) << "DxClusterClient: login prompt:" << line;
        m_socket->write((m_callsign + "\r\n").toLatin1());
        m_loggedIn = true;
        qCDebug(lcDxCluster) << "DxClusterClient: sent callsign" << m_callsign;
        sendStartupCommands();
        return;
    }

    // Try to parse as a DX spot
    DxSpot spot;
    if (parseDxSpotLine(line, spot)) {
        qCDebug(lcDxCluster) << "DxClusterClient: spot" << spot.dxCall
                 << spot.freqMhz << "MHz de" << spot.spotterCall;
        emit spotReceived(spot);
    }
}

// ── Startup commands replay ─────────────────────────────────────────────────

void DxClusterClient::sendStartupCommands()
{
    const QString raw = AppSettings::instance().value(m_startupCommandsKey).toString();
    if (raw.isEmpty()) return;
    const QStringList lines = raw.split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString cmd = line.trimmed();
        if (cmd.isEmpty()) continue;
        sendCommand(cmd);
    }
}

// ── Login prompt detection ──────────────────────────────────────────────────

bool DxClusterClient::isLoginPrompt(const QString& line) const
{
    // DX cluster servers vary: "login:", "call:", "callsign:", "Please enter your call",
    // "your call>", "Enter your callsign"
    QString lower = line.toLower();
    if (lower.endsWith("login:") || lower.endsWith("call:") || lower.endsWith("callsign:"))
        return true;
    if (lower.contains("enter your call") || lower.contains("your call"))
        return true;
    return false;
}

// ── DX spot line parser ─────────────────────────────────────────────────────

bool DxClusterClient::parseDxSpotLine(const QString& line, DxSpot& spot) const
{
    // Standard format: DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z
    // Z is the terminator — ignore any trailing chars (some nodes append BEL/NUL)
    static const QRegularExpression rx(
        R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
        QRegularExpression::CaseInsensitiveOption);

    auto match = rx.match(line);
    if (!match.hasMatch())
        return false;

    spot.spotterCall = match.captured(1);
    double freqKhz   = match.captured(2).toDouble();
    spot.freqMhz     = freqKhz / 1000.0;
    spot.dxCall       = match.captured(3);
    spot.comment      = match.captured(4).trimmed();

    QString timeStr = match.captured(5);
    int hh = timeStr.left(2).toInt();
    int mm = timeStr.mid(2, 2).toInt();
    spot.utcTime = QTime(hh, mm);

    return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
}

} // namespace AetherSDR
