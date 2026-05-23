#include "RadioConnection.h"
#include "LogManager.h"

#include <QEventLoop>

#ifdef Q_OS_LINUX
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(Q_OS_MACOS)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// MinGW headers may lack TCP_INFO_v0 / SIO_TCP_INFO (Windows 10 1703+)
#ifndef SIO_TCP_INFO
#define SIO_TCP_INFO _WSAIORW(IOC_VENDOR, 39)
typedef struct _TCP_INFO_v0 {
    ULONG    State;  // TCPSTATE enum — not used, just padding
    ULONG    Mss;
    ULONG64  ConnectionTimeMs;
    BOOLEAN  TimestampsEnabled;
    ULONG    RttUs;
    ULONG    MinRttUs;
    ULONG    BytesInFlight;
    ULONG    Cwnd;
    ULONG    SndWnd;
    ULONG    RcvWnd;
    ULONG    RcvBuf;
    ULONG64  BytesOut;
    ULONG64  BytesIn;
    ULONG    BytesReordered;
    ULONG    BytesRetrans;
    ULONG    FastRetrans;
    ULONG    DupAcksIn;
    ULONG    TimeoutEpisodes;
    UCHAR    SynRetrans;
} TCP_INFO_v0;
#endif
#endif

namespace AetherSDR {

static constexpr int HEARTBEAT_INTERVAL_MS = 30000;

// Cap the line-assembly buffer.  A buggy or hostile peer that dribbles bytes
// without ever sending '\n' would otherwise grow m_readBuffer unbounded until
// QByteArray refuses to allocate (process OOM).  CAT lines are tens of bytes;
// 16 MiB is wildly larger than any legitimate radio command or status burst.
// Mirrors the pattern from GHSA-7w4w-wfqm-wh93 (M2, RigctlServer) with a
// looser cap appropriate to the chattier status stream.
static constexpr int kMaxReadBuffer = 16 * 1024 * 1024;

static QString formatHexId(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 0, 16);
}

RadioConnection::RadioConnection(QObject* parent)
    : QObject(parent)
{
}

RadioConnection::~RadioConnection()
{
    disconnectFromRadio();
}

void RadioConnection::init()
{
    m_socket = new QTcpSocket(this);
    m_heartbeat = new QTimer(this);

    connect(m_socket, &QTcpSocket::connected,
            this, &RadioConnection::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &RadioConnection::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &RadioConnection::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &RadioConnection::onSocketError);

    m_heartbeat->setInterval(HEARTBEAT_INTERVAL_MS);
    connect(m_heartbeat, &QTimer::timeout, this, &RadioConnection::onHeartbeat);
}

void RadioConnection::connectToRadio(const RadioInfo& info)
{
    connectToHost(info.address, info.port,
                  info.bindSettings.mode,
                  info.bindSettings.bindAddress,
                  info.sessionBindAddress);
}

void RadioConnection::connectToHost(const QHostAddress& address,
                                    quint16 port,
                                    RadioBindMode bindMode,
                                    const QHostAddress& explicitBindAddr,
                                    const QHostAddress& sessionBindAddr)
{
    if (m_state.load() != ConnectionState::Disconnected) return;
    if (!m_socket) { qCWarning(lcConnection) << "RadioConnection: init() not called"; return; }

    m_bindMode = bindMode;
    m_explicitBindAddr = explicitBindAddr;
    m_sessionBindAddr = sessionBindAddr;
    m_radioAddr = address;
    m_localAddr = QHostAddress();
    m_localPort = 0;
    m_socket->abort();

    const QHostAddress preferredBindAddr =
        (bindMode == RadioBindMode::Explicit) ? explicitBindAddr : sessionBindAddr;
    const bool shouldBind =
        !preferredBindAddr.isNull() &&
        preferredBindAddr.protocol() == QAbstractSocket::IPv4Protocol;

    if (shouldBind) {
        if (!m_socket->bind(preferredBindAddr, 0)) {
            const QString msg = QStringLiteral("Failed to bind local TCP source address %1: %2")
                                    .arg(preferredBindAddr.toString(), m_socket->errorString());
            if (bindMode == RadioBindMode::Explicit) {
                qCWarning(lcConnection) << "RadioConnection:" << msg;
                m_socket->abort();
                m_socket->close();
                setState(ConnectionState::Error);
                emit errorOccurred(msg);
                return;
            }

            qCDebug(lcConnection) << "RadioConnection: auto bind to"
                                  << preferredBindAddr.toString()
                                  << "failed, falling back to OS routing:"
                                  << m_socket->errorString();
        } else {
            qCDebug(lcConnection) << "RadioConnection: bound local TCP source to"
                                  << preferredBindAddr.toString()
                                  << ":" << m_socket->localPort()
                                  << "(mode"
                                  << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                                  << ")";
        }
    } else {
        qCDebug(lcConnection) << "RadioConnection: no explicit local TCP bind, letting OS route"
                              << "(mode"
                              << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                              << ")";
    }

    qCDebug(lcConnection) << "RadioConnection: connecting to" << address.toString() << ":" << port;
    setState(ConnectionState::Connecting);
    m_socket->connectToHost(address, port);
}

void RadioConnection::disconnectFromRadio()
{
    if (m_heartbeat) m_heartbeat->stop();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        writeDisconnectMarker();
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(2000);
    }
    m_handle = 0;
}

void RadioConnection::gracefulDisconnect(quint32 handle,
                                         const QString& rxStreamId,
                                         quint32 streamRemoveSeq)
{
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState) {
        disconnectFromRadio();
        return;
    }

    if (m_heartbeat) m_heartbeat->stop();

    QElapsedTimer totalTimer;
    totalTimer.start();
    bool streamRemoveAcked = rxStreamId.isEmpty();
    qint64 streamRemoveMs = -1;

    // The radio needs to process teardown commands before TCP close begins.
    // Fire-and-forget C0 writes can leave the Flex session table stale (#2218).
    if (!rxStreamId.isEmpty() && streamRemoveSeq != 0) {
        QElapsedTimer stepTimer;
        stepTimer.start();
        streamRemoveAcked =
            sendCommandAndWait(streamRemoveSeq,
                               QString("stream remove 0x%1").arg(rxStreamId),
                               2000);
        streamRemoveMs = stepTimer.elapsed();
    }

    QElapsedTimer closeTimer;
    closeTimer.start();
    disconnectFromRadio();
    const qint64 closeMs = closeTimer.elapsed();

    qCInfo(lcConnection).nospace()
        << "RadioConnection: disconnect summary "
        << "handle=" << formatHexId(handle)
        << " streamId=" << (rxStreamId.isEmpty() ? QStringLiteral("<none>") : rxStreamId)
        << " streamSeq=" << streamRemoveSeq
        << " streamAck=" << (streamRemoveAcked ? "yes" : "no")
        << " streamMs=" << streamRemoveMs
        << " closeMs=" << closeMs
        << " totalMs=" << totalTimer.elapsed();
}

void RadioConnection::writeCommand(quint32 seq, const QString& command)
{
    if (!isConnected() || !m_socket) return;

    const QByteArray data = CommandParser::buildCommand(seq, command);
    if (command.startsWith("ping")) {
        m_lastPingSeq = seq;
        m_pingStopwatch.restart();  // fallback timer in case TCP_INFO unavailable
    } else {
        qCDebug(lcConnection) << "TX:" << data.trimmed();
    }
    m_socket->write(data);
    m_socket->flush();   // force immediate kernel send for keepalive reliability
}

void RadioConnection::onSocketConnected()
{
    qCDebug(lcConnection) << "RadioConnection: TCP connected";
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_radioAddr = m_socket->peerAddress();
    m_localAddr = m_socket->localAddress();
    m_localPort = m_socket->localPort();
    qCDebug(lcConnection) << "RadioConnection: local TCP endpoint"
                          << m_localAddr.toString() << ":" << m_localPort;
    qDebug() << "RTT method:" << (kernelRttMs() >= 0 ? "kernel TCP_INFO" : "QElapsedTimer fallback");
}

void RadioConnection::onSocketDisconnected()
{
    qCDebug(lcConnection) << "RadioConnection: TCP disconnected";
    if (m_heartbeat) m_heartbeat->stop();
    m_localAddr = QHostAddress();
    m_localPort = 0;
    setState(ConnectionState::Disconnected);
    emit disconnected();
}

void RadioConnection::onSocketError(QAbstractSocket::SocketError)
{
    if (!m_socket) return;
    const QString msg = m_socket->errorString();
    qCWarning(lcConnection) << "RadioConnection: socket error:" << msg;
    setState(ConnectionState::Error);
    emit errorOccurred(msg);
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        setState(ConnectionState::Disconnected);
}

void RadioConnection::onReadyRead()
{
    if (!m_socket) return;
    m_readBuffer.append(m_socket->readAll());
    if (m_readBuffer.size() > kMaxReadBuffer) {
        qCWarning(lcConnection) << "RadioConnection: read buffer exceeded"
                                << kMaxReadBuffer << "bytes without newline — disconnecting";
        m_socket->disconnectFromHost();
        m_readBuffer.clear();
        return;
    }
    int newlinePos;
    while ((newlinePos = m_readBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_readBuffer.left(newlinePos)).trimmed();
        m_readBuffer.remove(0, newlinePos + 1);
        if (!line.isEmpty())
            processLine(line);
    }
}

void RadioConnection::onHeartbeat()
{
}

void RadioConnection::processLine(const QString& line)
{
    const bool isGps = line.contains("|gps ");
    bool isPingReply = false;
    if (m_lastPingSeq && line.startsWith("R")) {
        isPingReply = line.startsWith(QString("R%1|").arg(m_lastPingSeq));
        if (isPingReply) {
            int rtt = kernelRttMs();
            if (rtt < 0)
                rtt = static_cast<int>(m_pingStopwatch.elapsed());  // fallback
            emit pingRttMeasured(rtt);
            m_lastPingSeq = 0;
        }
    }
    if (!isGps && !isPingReply)
        qCDebug(lcConnection) << "RX:" << line;

    ParsedMessage msg = CommandParser::parseLine(line);
    emit messageReceived(msg);

    switch (msg.type) {
    case MessageType::Version:
        emit versionReceived(msg.object);
        break;
    case MessageType::Handle:
        m_handle = msg.handle;
        qCDebug(lcConnection) << "RadioConnection: assigned handle" << QString::number(m_handle, 16);
        setState(ConnectionState::Connected);
        if (m_heartbeat) m_heartbeat->start();
        emit connected();
        break;
    case MessageType::Response:
        emit commandResponse(msg.sequence, msg.resultCode, msg.object);
        break;
    case MessageType::Status:
        emit statusReceived(msg.object, msg.kvs);
        break;
    default:
        break;
    }
}

// ─── Kernel-level TCP RTT ────────────────────────────────────────────────────
// Read the smoothed RTT maintained by the kernel's TCP congestion control.
// This is measured from TCP ACK round-trips at the kernel level, completely
// independent of Qt event loop buffering or application-layer timing.

int RadioConnection::kernelRttMs() const
{
    if (!m_socket) return -1;
    const auto fd = m_socket->socketDescriptor();
    if (fd == -1) return -1;

#ifdef Q_OS_LINUX
    struct tcp_info info{};
    socklen_t len = sizeof(info);
    if (getsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_INFO, &info, &len) == 0)
        return static_cast<int>(info.tcpi_rtt / 1000);  // µs → ms
#elif defined(Q_OS_MACOS)
    struct tcp_connection_info info{};
    socklen_t len = sizeof(info);
    if (getsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) == 0)
        return static_cast<int>(info.tcpi_srtt);  // already ms on macOS
#elif defined(Q_OS_WIN)
    TCP_INFO_v0 info{};
    DWORD infoLen = sizeof(info);
    DWORD version = 0;
    if (WSAIoctl(static_cast<SOCKET>(fd), SIO_TCP_INFO, &version, sizeof(version),
                 &info, sizeof(info), &infoLen, nullptr, nullptr) == 0)
        return static_cast<int>(info.RttUs / 1000);  // µs → ms
#endif

    return -1;  // unsupported platform or call failed
}

void RadioConnection::setState(ConnectionState s)
{
    m_state.store(s);
    emit stateChanged(s);
}

bool RadioConnection::sendCommandAndWait(quint32 seq, const QString& command, int timeoutMs)
{
    if (seq == 0 || !m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return false;

    bool received = false;
    int responseCode = 0;
    QString responseBody;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    const QMetaObject::Connection responseConn =
        connect(this, &RadioConnection::commandResponse, &loop,
                [&](quint32 responseSeq, int code, const QString& body) {
            if (responseSeq != seq)
                return;
            received = true;
            responseCode = code;
            responseBody = body;
            loop.quit();
        });

    const QMetaObject::Connection disconnectedConn =
        connect(m_socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);

    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    writeCommand(seq, command);
    timer.start(timeoutMs);
    loop.exec();

    disconnect(responseConn);
    disconnect(disconnectedConn);

    if (!received) {
        qCWarning(lcConnection) << "RadioConnection: timed out waiting for teardown response"
                                << "seq" << seq << "command" << command;
        return false;
    }

    qCDebug(lcConnection) << "RadioConnection: teardown response"
                          << "seq" << seq
                          << "code" << Qt::hex << responseCode
                          << "body" << responseBody;
    return true;
}

void RadioConnection::writeDisconnectMarker()
{
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return;

    m_socket->write(QByteArray(1, '\x04'));
    m_socket->flush();
    m_socket->waitForBytesWritten(250);
}

} // namespace AetherSDR
