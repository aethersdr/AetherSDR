#include "SpeConnection.h"
#include "LogManager.h"

namespace AetherSDR {

SpeConnection::SpeConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &SpeConnection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &SpeConnection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &SpeConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    m_parser.setFrameCallback([this](const Spe::Frame& f) { onFrameReceived(f); });

    // Retries every 5s indefinitely until the amp returns or the user
    // disconnects — same cadence as the other peripheral connections
    // (Pgxl/Tgxl/Acom) for a device that may be power-cycling or unplugged.
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected) { return; }
        if (m_mode == Mode::Network && !m_lastHost.isEmpty()) {
            connectNetwork(m_lastHost, m_lastPort);
#ifdef HAVE_SERIALPORT
        } else if (m_mode == Mode::Serial && !m_lastSerialPort.isEmpty()) {
            connectSerial(m_lastSerialPort);
#endif
        }
    });

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &SpeConnection::pollTick);
}

QString SpeConnection::description() const
{
    if (m_mode == Mode::Network) {
        return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
    }
#ifdef HAVE_SERIALPORT
    if (m_mode == Mode::Serial) {
        return m_lastSerialPort;
    }
#endif
    return QString();
}

QString SpeConnection::sourceLabel() const
{
    switch (m_mode) {
        case Mode::Network: return QStringLiteral("NETWORK");
        case Mode::Serial:  return QStringLiteral("SERIAL");
        default:            return QStringLiteral("—");
    }
}

#ifdef HAVE_SERIALPORT
void SpeConnection::connectSerial(const QString& portName)
{
    m_mode = Mode::Serial;
    m_lastSerialPort = portName;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    if (!m_serialPort) {
        m_serialPort = new QSerialPort(this);
        connect(m_serialPort, &QSerialPort::readyRead, this, &SpeConnection::onReadyRead);
        connect(m_serialPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError err) {
            if (err == QSerialPort::NoError) { return; }
            const QString msg = m_serialPort->errorString();
            qCWarning(lcTuner) << "SpeConnection: serial error" << err << msg;
            onTransportError(msg);
            if (m_connected) { onTransportDown(); }
        });
    }

    m_serialPort->setPortName(portName);
    // 115200 8N1 no handshake — the spec's documented maximum; the amp
    // auto-adapts to lower speeds so there's nothing to configure.
    m_serialPort->setBaudRate(QSerialPort::Baud115200);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        const QString err = m_serialPort->errorString();
        qCWarning(lcTuner) << "SpeConnection: failed to open" << portName << err;
        emit connectionFailed(err);
        if (m_autoReconnect) { armReconnect(); }
        return;
    }
    // Hold DTR/RTS low — on the Expert the power-switch line can be driven
    // through the serial interface's control signals (that is how remote
    // power-on works), so leaving them asserted by default risks holding
    // the amp's power switch. Same precaution AcomConnection takes.
    m_serialPort->setDataTerminalReady(false);
    m_serialPort->setRequestToSend(false);

    m_device = m_serialPort;
    onTransportUp();
}
#endif

void SpeConnection::connectNetwork(const QString& host, quint16 port)
{
    m_mode = Mode::Network;
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    m_device = &m_socket;
    qCDebug(lcTuner) << "SpeConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
}

void SpeConnection::disconnect()
{
    // Self-contained rather than relying on teardownDevice() to indirectly
    // trigger onTransportDown() — QTcpSocket::abort() emits disconnected()
    // synchronously but QSerialPort::close() emits nothing, and that
    // asymmetry left AcomConnection's applet stuck at "Connected" on a
    // user-initiated serial disconnect until it was made self-contained.
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_connected = false;
    teardownDevice();
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "SpeConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void SpeConnection::teardownDevice()
{
    if (m_device == &m_socket && m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
#ifdef HAVE_SERIALPORT
    if (m_serialPort && m_device == m_serialPort && m_serialPort->isOpen()) {
        m_serialPort->close();
    }
#endif
    m_device = nullptr;
}

void SpeConnection::onTransportUp()
{
    m_connected = true;
    m_statusSeenSinceTick = false;
    m_silentPolls = 0;
    m_responding = false;
    m_currentModelId.clear();
    qCInfo(lcTuner) << "SpeConnection: connected via" << description();

    // First poll immediately — the timer only fires after a full interval,
    // and the applet shouldn't sit blank for it.
    sendRaw(Spe::buildStatusRequest());
    m_pollTimer.start();

    emit connected();
}

void SpeConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_pollTimer.stop();
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "SpeConnection: disconnected";
        emit disconnected();
    }
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void SpeConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "SpeConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
        armReconnect();
    }
}

void SpeConnection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void SpeConnection::onReadyRead()
{
    if (!m_device) { return; }
    m_parser.feed(m_device->readAll());
}

void SpeConnection::pollTick()
{
    // Silence detection first: with ser2net the TCP link happily outlives
    // the amplifier being switched off, so unanswered polls — not a socket
    // drop — are the only "amp went away" evidence that topology produces.
    if (!m_statusSeenSinceTick) {
        if (++m_silentPolls == kSilentPollLimit && m_responding) {
            qCWarning(lcTuner) << "SpeConnection: amplifier stopped answering status"
                                  " polls (link is up) — switched off, or not an SPE"
                                  " on this port? Polling continues.";
            m_responding = false;
            emit respondingChanged(false);
        }
    } else {
        m_silentPolls = 0;
    }
    m_statusSeenSinceTick = false;

    sendRaw(Spe::buildStatusRequest());
}

void SpeConnection::onFrameReceived(const Spe::Frame& f)
{
    if (f.isAck()) {
        // Keystroke echo — logged for command traceability, nothing to
        // update: the next status poll reflects any resulting state change
        // within one poll interval.
        qCDebug(lcTuner) << "SpeConnection: ACK for command"
                          << QString::number(static_cast<quint8>(f.data.at(0)), 16);
        return;
    }

    const auto status = Spe::parseStatus(f.data);
    if (!status) {
        qCWarning(lcTuner) << "SpeConnection: unparseable status reply ("
                            << f.data.size() << "B) —" << f.data.left(24);
        return;
    }

    m_statusSeenSinceTick = true;
    m_silentPolls = 0;
    if (!m_responding) {
        m_responding = true;
        emit respondingChanged(true);
    }

    m_lastStatus = *status;
    if (status->id != m_currentModelId) {
        m_currentModelId = status->id;
        qCInfo(lcTuner) << "SpeConnection: amplifier identifies as" << m_currentModelId
                         << "(" << Spe::modelSpec(m_currentModelId).displayName << ")";
        emit modelChanged(m_currentModelId);
    }
    emit statusUpdated(*status);
}

void SpeConnection::sendKey(Spe::Key key)
{
    sendRaw(Spe::buildKeyCommand(key));
}

void SpeConnection::sendRaw(const QByteArray& packet)
{
    if (!m_device || !m_connected) { return; }
    m_device->write(packet);
}

}  // namespace AetherSDR
