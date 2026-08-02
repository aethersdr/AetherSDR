#include "VkampConnection.h"
#include "LogManager.h"

namespace AetherSDR {

VkampConnection::VkampConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &VkampConnection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &VkampConnection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &VkampConnection::onTcpReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    connect(&m_udpSocket, &QUdpSocket::readyRead, this, &VkampConnection::onUdpReadyRead);

    m_parser.setStatusCallback([this](const Vkamp::Status& s) { onStatusReceived(s); });

    // Retries every 5s indefinitely until the amp returns or the user
    // disconnects -- matches PgxlConnection/TgxlConnection/AcomConnection's
    // precedent for a peripheral that may be power-cycling.
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected) { return; }
        if (!m_lastHost.isEmpty()) {
            connectNetwork(m_lastHost, m_lastPort, m_lastUdpPort);
        }
    });

    // The amp is request/response for status while idle -- confirmed live,
    // 20+ seconds of pure idle silence produced zero replies (design doc
    // Section 6). This ping is the only thing that produces one.
    m_keepaliveTimer.setInterval(kKeepaliveIntervalMs);
    connect(&m_keepaliveTimer, &QTimer::timeout, this, [this]() {
        sendCommand(Vkamp::buildPoll());
    });

    // Dead-link watchdog -- restarted only when a real status reply lands
    // (onStatusReceived), not on every byte read. A real disconnect is
    // expected to arrive as a clean socket close; this is the fallback for
    // a link that goes silently dark (design doc Section 6).
    m_staleStatusTimer.setSingleShot(true);
    m_staleStatusTimer.setInterval(kStaleStatusTimeoutMs);
    connect(&m_staleStatusTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        qCWarning(lcTuner) << "VkampConnection: no status received in"
                            << kStaleStatusTimeoutMs / 1000.0 << "s -- treating link as dead";
        m_socket.abort();  // triggers onTransportDown() via disconnected()
    });

    // UDP telemetry only flows while re-triggered periodically, on the SAME
    // socket for the life of the connection (design doc Section 3.2) --
    // churning sockets/ports risks losing telemetry right as a TX starts.
    m_telemetryTriggerTimer.setInterval(kTelemetryRetriggerMs);
    connect(&m_telemetryTriggerTimer, &QTimer::timeout, this, [this]() {
        m_udpSocket.writeDatagram(Vkamp::buildPoll(), QHostAddress(m_lastHost), m_lastUdpPort);
    });
}

QString VkampConnection::description() const
{
    if (m_lastHost.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
}

void VkampConnection::connectNetwork(const QString& host, quint16 port, quint16 udpPort)
{
    m_lastHost = host;
    m_lastPort = port;
    m_lastUdpPort = udpPort;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    m_parser.reset();

    qCDebug(lcTuner) << "VkampConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
}

void VkampConnection::disconnect()
{
    // Self-contained, same reasoning as AcomConnection::disconnect(): don't
    // depend on the socket's own disconnected() signal to drive this cleanup,
    // since a user-initiated abort() sometimes fires it synchronously and
    // sometimes doesn't, which would otherwise leave VkampApplet stuck
    // showing "Connected".
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_keepaliveTimer.stop();
    m_staleStatusTimer.stop();
    m_telemetryTriggerTimer.stop();
    if (m_resetTimer) { m_resetTimer->stop(); }
    m_resetting = false;
    m_connected = false;
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "VkampConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void VkampConnection::onTransportUp()
{
    m_connected = true;
    m_lastStatus = Vkamp::Status();
    qCInfo(lcTuner) << "VkampConnection: connected to" << description();

    sendCommand(Vkamp::buildPoll());
    m_udpSocket.writeDatagram(Vkamp::buildPoll(), QHostAddress(m_lastHost), m_lastUdpPort);

    m_keepaliveTimer.start();
    m_staleStatusTimer.start();
    m_telemetryTriggerTimer.start();

    emit connected();
}

void VkampConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_keepaliveTimer.stop();
    m_staleStatusTimer.stop();
    m_telemetryTriggerTimer.stop();
    if (m_resetTimer) { m_resetTimer->stop(); }
    m_resetting = false;
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "VkampConnection: disconnected";
        emit disconnected();
    }
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void VkampConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "VkampConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
        armReconnect();
    }
}

void VkampConnection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void VkampConnection::onTcpReadyRead()
{
    m_parser.feed(m_socket.readAll());
}

void VkampConnection::onUdpReadyRead()
{
    while (m_udpSocket.hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_udpSocket.pendingDatagramSize()));
        m_udpSocket.readDatagram(data.data(), data.size());
        const auto telemetry = Vkamp::parseTelemetry(data);
        if (telemetry) {
            emit telemetryUpdated(*telemetry);
        }
    }
}

void VkampConnection::onStatusReceived(const Vkamp::Status& status)
{
    m_lastStatus = status;
    m_staleStatusTimer.start();  // restart -- see this timer's own setup comment
    emit statusUpdated(status);
}

bool VkampConnection::sendCommand(const QByteArray& code)
{
    if (!m_connected) {
        qCWarning(lcTuner) << "VkampConnection: command" << code << "dropped -- not connected";
        return false;
    }
    if (m_lastCommandTimer.isValid() && m_lastCommandTimer.elapsed() < kMinCommandIntervalMs) {
        // Protects the amp's relay/PSU from a rapid-fire burst -- see this
        // guard's own doc comment in VkampConnection.h. Silently held back,
        // not dropped with a warning: unlike "not connected", a command
        // clicked slightly too fast is expected/harmless to just skip once.
        qCDebug(lcTuner) << "VkampConnection: command" << code << "held back -- within"
                          << kMinCommandIntervalMs << "ms of the last one";
        return false;
    }
    m_socket.write(code);
    m_lastCommandTimer.restart();
    return true;
}

void VkampConnection::setBypass(bool on)
{
    sendCommand(Vkamp::buildBypass(on));
}

void VkampConnection::setCoolingOverride(bool on)
{
    sendCommand(Vkamp::buildCooling(on));
}

void VkampConnection::setVoltage(bool low)
{
    if (m_lastStatus.bypass) {
        // Real-hardware safety finding (design doc Section 5): commanding a
        // rail change while bypassed was observed live pulling the amp's
        // supply toward ~0V. This is a second guard below VkampApplet's own
        // disabled-button gating, not a substitute for it.
        qCWarning(lcTuner) << "VkampConnection: voltage command refused -- amp is bypassed";
        return;
    }
    sendCommand(Vkamp::buildVoltage(low));
}

void VkampConnection::selectAntenna(int port)
{
    if (port < 1 || port > 3) {
        qCWarning(lcTuner) << "VkampConnection: antenna port" << port << "out of range (1-3), ignoring";
        return;
    }
    sendCommand(Vkamp::buildSelectAntenna(port));
}

void VkampConnection::startReset()
{
    if (m_resetting || !m_connected) {
        return;
    }
    m_resetting = true;
    m_resetRemaining = kResetHoldSeconds;

    if (!m_resetTimer) {
        m_resetTimer = new QTimer(this);
        m_resetTimer->setInterval(static_cast<int>(kResetIntervalSeconds * 1000));
        connect(m_resetTimer, &QTimer::timeout, this, [this]() {
            sendCommand(Vkamp::buildResetHold());
            m_resetRemaining -= kResetIntervalSeconds;
            if (m_resetRemaining <= 0.0) {
                m_resetTimer->stop();
                m_resetting = false;
                emit resetProgress(0.0);
                emit resetFinished();
                return;
            }
            emit resetProgress(m_resetRemaining);
        });
    }

    // Send the first hold immediately -- matches the companion project's own
    // reset() loop, which sends on every interval including the first,
    // rather than waiting one full interval before the amp sees anything.
    sendCommand(Vkamp::buildResetHold());
    emit resetProgress(m_resetRemaining);
    m_resetTimer->start();
}

}  // namespace AetherSDR
