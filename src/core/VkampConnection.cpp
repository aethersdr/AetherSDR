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
        // Held back during a reset hold by sendCommand()'s own gate.
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
        sendTelemetryTrigger();
    });

    // Telemetry is TX-gated (design doc Section 3.2): the amp streams while
    // transmitting and is silent otherwise, so "frames stopped" is the normal
    // end of a transmission, not a fault. Without this the last frame of the
    // last transmission stays on the gauges for the whole idle period --
    // forward power, SWR and current all reading as though the amp were still
    // keyed. Every other amplifier applet is spared this only because its amp
    // keeps streaming zeros at idle; this one never will.
    m_telemetryStallTimer.setSingleShot(true);
    m_telemetryStallTimer.setInterval(kTelemetryStallMs);
    connect(&m_telemetryStallTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        qCDebug(lcTuner) << "VkampConnection: telemetry idle -- expiring the last frame";
        emit telemetryStalled();
    });
}

void VkampConnection::sendTelemetryTrigger()
{
    if (m_resolvedHost.isNull()) {
        return;
    }
    if (m_udpSocket.writeDatagram(Vkamp::buildPoll(), m_resolvedHost, m_lastUdpPort) < 0) {
        qCWarning(lcTuner) << "VkampConnection: telemetry trigger to" << m_resolvedHost
                           << "failed --" << m_udpSocket.errorString();
    }
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

    // Abort any attempt still in flight first -- a bare connectToHost() on a
    // socket that isn't UnconnectedState (e.g. a prior click still stuck in
    // HostLookupState/ConnectingState, or mid-teardown from a just-failed
    // attempt) is silently ignored by Qt rather than restarting the dial,
    // which is what made Connect need several clicks before one landed in a
    // clean state. Same guard AcomConnection::teardownDevice() already uses.
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }

    qCDebug(lcTuner) << "VkampConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).

    const int epoch = ++m_connectEpoch;
    QTimer::singleShot(kConnectTimeoutMs, this, [this, epoch]() {
        if (m_connectEpoch != epoch) { return; }  // superseded by a later attempt
        if (m_connected || m_socket.state() == QAbstractSocket::ConnectedState) { return; }
        qCWarning(lcTuner) << "VkampConnection: connect timeout after" << kConnectTimeoutMs << "ms";
        m_socket.abort();
        emit connectionFailed(QStringLiteral("Connect timed out"));
        if (!m_deliberateDisconnect && m_autoReconnect) {
            armReconnect();
        }
    });
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
    ++m_connectEpoch;  // invalidates any pending connect-timeout watchdog
    m_reconnectTimer.stop();
    m_keepaliveTimer.stop();
    m_staleStatusTimer.stop();
    m_telemetryTriggerTimer.stop();
    m_telemetryStallTimer.stop();
    m_resolvedHost.clear();
    // The next connection starts with a clean rate-limit budget -- otherwise
    // a command sent just before the drop can hold back the reconnect poll.
    m_lastCommandTimer.invalidate();
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

    // Every command this protocol sends is a bare 2-byte ASCII code -- no
    // bulk data to gain from Nagle's own coalescing, only its latency cost
    // (tens-to-hundreds of ms once it interacts with the amp's own
    // delayed-ACK timer). Same fix and reasoning as the companion
    // vkamp_client.py's own connect(), which sets TCP_NODELAY for the same
    // reason on every connection, not just a slow/remote one.
    m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);

    // Reuse whatever the TCP connect already resolved rather than re-parsing
    // the user's string -- see m_resolvedHost's own doc comment for why
    // QHostAddress(m_lastHost) is not equivalent.
    m_resolvedHost = m_socket.peerAddress();
    if (m_resolvedHost.isNull()) {
        qCWarning(lcTuner) << "VkampConnection: connected but peer address is null --"
                              "UDP telemetry will not flow";
    }

    sendCommand(Vkamp::buildPoll());
    sendTelemetryTrigger();

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
    m_telemetryStallTimer.stop();
    m_resolvedHost.clear();
    // The next connection starts with a clean rate-limit budget -- otherwise
    // a command sent just before the drop can hold back the reconnect poll.
    m_lastCommandTimer.invalidate();
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
            // Below the lowest raw count the output calibration curve was
            // ever actually fit to (~133W true output -- VkampProtocol.cpp's
            // own kOutputCalMinRaw doc comment), outputWatts() correctly
            // returns 0 rather than extrapolate into a bogus reading -- but
            // that also means real sub-~130W raw counts go unrecorded
            // anywhere right now. Logged at debug level purely so a real
            // external-meter reading at this raw count can become a new
            // calibration point later, same evidence bar as the existing
            // ones (design doc Section 3.2) -- never a constant invented
            // from this log alone.
            if (telemetry->output > 0 && telemetry->outputWatts() <= 0.0f) {
                qCDebug(lcTuner) << "VkampConnection: raw output" << telemetry->output
                                  << "below the calibrated floor -- pair with an "
                                     "external reference reading at this drive level "
                                     "to extend the curve";
            }
            m_telemetryStallTimer.start();  // see this timer's own setup comment
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

bool VkampConnection::sendCommand(const QByteArray& code, bool isResetHold)
{
    if (!m_connected) {
        qCWarning(lcTuner) << "VkampConnection: command" << code << "dropped -- not connected";
        return false;
    }
    if (m_resetting && !isResetHold) {
        // See sendCommand()'s own doc comment in the header: the reset hold
        // owns the wire until it finishes.
        qCWarning(lcTuner) << "VkampConnection: command" << code
                           << "dropped -- a reset hold is in progress";
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
            sendCommand(Vkamp::buildResetHold(), /*isResetHold=*/true);
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
    sendCommand(Vkamp::buildResetHold(), /*isResetHold=*/true);
    emit resetProgress(m_resetRemaining);
    m_resetTimer->start();
}

}  // namespace AetherSDR
