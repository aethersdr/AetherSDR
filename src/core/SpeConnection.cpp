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

    m_powerOnTimer.setSingleShot(true);
    connect(&m_powerOnTimer, &QTimer::timeout, this, &SpeConnection::powerOnStep);
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
    // Idle line state: DTR HIGH, RTS low — matching the power-ON pulse's
    // terminal step, so the resting state no longer depends on whether ON
    // was pressed this session and a reconnect produces no edge on either
    // line. Bench-ruled on a real 1.5K-FA (design note §9): with DTR held
    // high the amplifier raises no `R` ("Power switch held by remote")
    // warning, keystrokes including SWITCH OFF work normally, and repeated
    // power cycles behave — the power switch rides the RTS pulse alone,
    // which is why RTS (and only RTS) must stay low at rest.
    m_serialPort->setDataTerminalReady(true);
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
    m_powerOnTimer.stop();
    m_powerOnStep = -1;
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
    // Renegotiated per connection — a proxy reconfigured between sessions
    // must not be judged on the previous session's answer (nor on the
    // previous session's carried scan tail).
    m_comPortOption = Spe::Rfc2217::OptionReply::None;
    m_rfc2217Tail.clear();
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
    m_powerOnTimer.stop();
    m_powerOnStep = -1;
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
    const QByteArray chunk = m_device->readAll();

    // Watch for the proxy's answer to our WILL COM-PORT-OPTION before the
    // bytes go to the frame parser. Read-only — the parser resyncs past
    // negotiation on its own, so nothing is consumed here; this only records
    // whether RFC 2217 control is actually available, which powerOn() needs
    // to know before it claims the pulse reached the amplifier.
    //
    // Scanned over the previous read's 2-byte tail + this chunk: the 3-byte
    // DO/DONT sequence can straddle a TCP segment boundary, and a stateless
    // per-chunk scan would miss it — reporting "never confirmed" against a
    // correctly configured proxy. Only the tail is carried, never re-scanning
    // whole chunks, so a reply can't be double-counted either.
    if (m_mode == Mode::Network) {
        const auto reply = Spe::Rfc2217::scanComPortOptionReply(m_rfc2217Tail + chunk);
        m_rfc2217Tail = chunk.right(2);
        if (reply != Spe::Rfc2217::OptionReply::None && reply != m_comPortOption) {
            m_comPortOption = reply;
            if (reply == Spe::Rfc2217::OptionReply::Accepted) {
                qCInfo(lcTuner) << "SpeConnection: proxy accepted RFC 2217"
                                   " COM-port control — remote power-ON is"
                                   " available.";
            } else {
                qCWarning(lcTuner) << "SpeConnection: proxy REFUSED RFC 2217"
                                      " COM-port control. Monitoring and"
                                      " keystrokes still work, but remote"
                                      " power-ON needs the ser2net port as"
                                      " `accepter: telnet(rfc2217=true),<port>`.";
            }
        }
    }

    m_parser.feed(chunk);
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

void SpeConnection::setControlLines(bool dtr, bool rts)
{
    if (m_mode == Mode::Network) {
        // The proxy's serial lines, driven remotely via RFC 2217.
        sendRaw(Spe::Rfc2217::buildSetControl(
            dtr ? Spe::Rfc2217::kDtrOn : Spe::Rfc2217::kDtrOff));
        sendRaw(Spe::Rfc2217::buildSetControl(
            rts ? Spe::Rfc2217::kRtsOn : Spe::Rfc2217::kRtsOff));
#ifdef HAVE_SERIALPORT
    } else if (m_mode == Mode::Serial && m_serialPort && m_serialPort->isOpen()) {
        m_serialPort->setDataTerminalReady(dtr);
        m_serialPort->setRequestToSend(rts);
#endif
    }
}

void SpeConnection::powerOn()
{
    if (!m_connected || m_powerOnStep >= 0) { return; }
    qCInfo(lcTuner) << "SpeConnection: sending power-ON pulse via" << sourceLabel();
    m_powerOnStep = 0;
    if (m_mode == Mode::Network) {
        // Ask the proxy to interpret RFC 2217 frames, then give it a moment
        // — the reference application's own working pacing.
        sendRaw(Spe::Rfc2217::buildWillComPortOption());
        m_powerOnTimer.start(500);
    } else {
        powerOnStep();  // local serial lines need no negotiation
    }
}

void SpeConnection::powerOnStep()
{
    // Pulse sequence carried verbatim from the reference application:
    // DTR on (100 ms), then DTR off + RTS on (1000 ms — the actual power
    // pulse), then DTR on + RTS off to idle.
    switch (m_powerOnStep) {
        case 0:
            setControlLines(true, false);
            m_powerOnStep = 1;
            m_powerOnTimer.start(100);
            break;
        case 1:
            setControlLines(false, true);
            m_powerOnStep = 2;
            m_powerOnTimer.start(1000);
            break;
        case 2:
            setControlLines(true, false);
            m_powerOnStep = -1;
            // Report what was actually confirmed rather than assuming. The
            // pulse is always sent: a proxy that ignores COM-port control
            // simply discards the SET-CONTROL frames (or, in raw mode,
            // forwards them to the amp, which rejects them as unframed
            // noise), so sending is harmless — claiming it landed is not.
            if (m_mode == Mode::Network
                && m_comPortOption == Spe::Rfc2217::OptionReply::Refused) {
                // "May", not "did not": ser2net 4.3.11 with a plain
                // `accepter: telnet` port answers DONT yet still executes
                // SET-CONTROL — bench-verified on a real 1.5K-FA (design
                // note §9). The explicit rfc2217=true config remains the
                // recommendation because that behaviour is unspecified.
                qCWarning(lcTuner) << "SpeConnection: power-ON pulse sent, but"
                                      " the proxy REFUSED RFC 2217 COM-port"
                                      " control, so it may not have reached"
                                      " the amplifier (some ser2net builds"
                                      " act on it anyway — watch whether"
                                      " status polls resume). Recommended"
                                      " config: `accepter:"
                                      " telnet(rfc2217=true),<port>`.";
            } else if (m_mode == Mode::Network
                       && m_comPortOption != Spe::Rfc2217::OptionReply::Accepted) {
                qCWarning(lcTuner) << "SpeConnection: power-ON pulse sent, but"
                                      " the proxy never confirmed RFC 2217"
                                      " COM-port control — if the amplifier"
                                      " stays silent, check that ser2net runs"
                                      " this port as `accepter:"
                                      " telnet(rfc2217=true),<port>` rather"
                                      " than raw.";
            } else {
                qCInfo(lcTuner) << "SpeConnection: power-ON pulse complete — the"
                                   " amp should begin answering status polls"
                                   " shortly.";
            }
            break;
        default:
            m_powerOnStep = -1;
            break;
    }
}

void SpeConnection::sendRaw(const QByteArray& packet)
{
    if (!m_device || !m_connected) { return; }
    m_device->write(packet);
}

}  // namespace AetherSDR
