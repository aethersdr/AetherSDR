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
    m_legacyParser.setFrameCallback([this](const Spe::Legacy::Frame& f) {
        onLegacyFrameReceived(f);
    });
    m_parser.setDisplayCallback([this](const QByteArray& raw) {
        // LCD freshness and Status liveness are deliberately independent:
        // a moving display must not keep stale telemetry/buttons looking live.
        // The guard covers the emit too, so nothing is delivered while
        // polling is off. It does NOT correlate: a reply already in the
        // buffer when the presentation flips back can still repaint once
        // polling resumes — setLcdPolling() deliberately leaves m_parser
        // alone, since resetting it mid-connection would also discard a
        // partially-received Status frame.
        if (const auto frame = Spe::Lcd::decode(raw)) {
            if (m_lcdWanted && m_connected) {
                emit lcdFrameReceived(*frame);
                setLcdFresh(true);
                m_lcdStaleTimer.start();
                // Pace the next request from the REPLY so the amp's variable latency keeps
                // it from phase-locking with the 100 ms Status poll (evenly dividing timer
                // periods coalesce under Qt's coarse timers); kLcdPollIntervalMs is then
                // just an idle gap.
                applyLcdEffect(m_lcdScheduler.replyValid());
            }
        }
    });

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

    m_offReleaseTimer.setSingleShot(true);
    m_offReleaseTimer.setInterval(kLegacyOffReleaseMs);
    connect(&m_offReleaseTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected || !isLegacy()) { return; }
        // A raw bridge never moved the line, and SET-CONTROL frames sent
        // into one only reach the amp as noise.
        if (m_mode == Mode::Serial || m_legacyDtrRaised) {
            setControlLines(false, false);
        }
        m_legacyDtrRaised = false;
    });

    // ONE timer, single-shot, always armed with an explicit interval by
    // applyLcdEffect() for whichever role the scheduler assigned it (idle
    // gap, lost-reply fallback, or reject-retry pause). Request pacing
    // itself lives in Spe::LcdScheduler — a deterministic, I/O-free state
    // machine every trigger path flows through, so requests stay
    // single-file by construction and the property is unit-tested in
    // spe_protocol_test rather than asserted in comments.
    m_lcdTimer.setSingleShot(true);
    connect(&m_lcdTimer, &QTimer::timeout, this, [this]() {
        applyLcdEffect(m_lcdScheduler.timerFired());
    });

    m_lcdStaleTimer.setSingleShot(true);
    m_lcdStaleTimer.setInterval(kLcdStaleTimeoutMs);
    connect(&m_lcdStaleTimer, &QTimer::timeout, this, [this]() {
        // Routine on best-effort links (a stall, an amp quiet spell, RF
        // mid-transmit) — logged so field reports can measure the gaps.
        qCDebug(lcTuner) << "SpeConnection: no valid display frame for"
                         << kLcdStaleTimeoutMs << "ms — menu keys gated until"
                            " the next one";
        setLcdFresh(false);
    });

    // Re-request a corrupted display frame promptly (RF near the serial run
    // corrupts long display replies far more than Status). The scheduler resolves
    // the outstanding request and replaces its lost-reply fallback with the short
    // retry pause, so a late frame can't leave two timers racing. Each retry
    // needs a full received frame, so it self-limits.
    m_parser.setDisplayRejectCallback([this]() {
        if (!m_lcdWanted || !m_connected) {
            return;
        }
        qCDebug(lcTuner) << "SpeConnection: display frame failed validation —"
                            " re-requesting";
        applyLcdEffect(m_lcdScheduler.replyRejected());
    });
}

SpeConnection::~SpeConnection()
{
    // Members are destroyed after this body and the owner may be mid-
    // destruction, so nothing is emitted; disconnect() still sends RCU_OFF
    // and teardownDevice() releases the line explicitly — the OS close
    // behaviour differs by platform and driver (design note §12).
    blockSignals(true);
    disconnect();
}

void SpeConnection::applyLcdEffect(const Spe::LcdScheduler::Effect& effect)
{
    if (effect.sendRequest) {
        sendRaw(Spe::Lcd::buildRequest());
    }
    switch (effect.arm) {
        case Spe::LcdScheduler::Timer::IdleGap:
            m_lcdTimer.start(kLcdPollIntervalMs);
            break;
        case Spe::LcdScheduler::Timer::LostReply:
            m_lcdTimer.start(kLcdLostReplyMs);
            break;
        case Spe::LcdScheduler::Timer::RejectRetry:
            m_lcdTimer.start(kLcdRetryGapMs);
            break;
        case Spe::LcdScheduler::Timer::None:
            break;  // leave the armed timer alone — never a stop
    }
}

void SpeConnection::setLcdPolling(bool on)
{
    if (on == m_lcdWanted) {
        return;
    }
    m_lcdWanted = on;
    if (on && m_connected && !isLegacy()) {
        setLcdFresh(false);
        applyLcdEffect(m_lcdScheduler.enable());  // first refresh, no full wait
    } else {
        m_lcdScheduler.reset();
        m_lcdTimer.stop();
        m_lcdStaleTimer.stop();
        setLcdFresh(false);
    }
}

void SpeConnection::setLcdFresh(bool fresh)
{
    if (fresh == m_lcdFresh) {
        return;
    }
    m_lcdFresh = fresh;
    emit lcdFreshChanged(fresh);
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
    teardownDevice();  // under the previous variant's line rules
    m_activeVariant = m_variant;
    m_legacyDtrRaised = false;
    m_parser.reset();
    m_legacyParser.reset();

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
    // 115200 for the newer family (the spec's maximum; the amp auto-adapts),
    // 9600 for the original 1K-FA, which supports nothing else.
    m_serialPort->setBaudRate(Spe::serialBaud(m_activeVariant));
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);
    // 1K-FA: DTR is its power switch, so the line must not depend on what
    // close() restores. Windows restores the DCB captured before open()
    // (qserialport_win.cpp), whose DTR setting predates AetherSDR; POSIX
    // restores the original termios. teardownDevice() lowers the line
    // explicitly instead. The newer family keeps Qt's default.
    m_serialPort->setSettingsRestoredOnClose(!isLegacy());

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
    // The original 1K-FA is the exception: there DTR held high IS the power
    // switch, so both lines go low at once. AetherSDR never raises DTR on
    // connect — only ON does. Caveat, reasoned from the kernels and not yet
    // measured: Linux and macOS assert DTR inside open() itself, so the line
    // can be high for the moment until this call; whether such a blip starts
    // a 1K-FA is an open bench question (design note §12).
    if (isLegacy()) {
        m_serialPort->setDataTerminalReady(false);
        m_serialPort->setRequestToSend(false);
    } else {
        m_serialPort->setDataTerminalReady(true);
        m_serialPort->setRequestToSend(false);
    }

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
    teardownDevice();  // under the previous variant's line rules
    m_activeVariant = m_variant;
    m_legacyDtrRaised = false;
    m_parser.reset();
    m_legacyParser.reset();

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
    if (wasConnected && isLegacy()) {
        // Stop the Status stream so the amp is quiet for the next client.
        sendRaw(Spe::Legacy::buildRcuOff());
        drainWrites();
    }
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_lcdScheduler.reset();
    m_lcdTimer.stop();
    m_lcdStaleTimer.stop();
    setLcdFresh(false);
    m_powerOnTimer.stop();
    m_powerOnStep = -1;
    m_offReleaseTimer.stop();  // teardownDevice() releases the line anyway
    m_rfc2217NegotiationPending = false;
    m_connected = false;
    teardownDevice();
    m_parser.reset();
    m_legacyParser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "SpeConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void SpeConnection::teardownDevice()
{
    // 1K-FA line policy (design note §12): DTR — its power switch — is held
    // only while AetherSDR has the port open. It is lowered explicitly here,
    // before every close (Disconnect, reconnect, quit), rather than left to
    // the platform: what close() does to DTR differs between Windows (DCB
    // restore, then the driver) and Linux/macOS (HUPCL), so only an explicit
    // release behaves the same everywhere.
    if (m_device == &m_socket && m_socket.state() != QAbstractSocket::UnconnectedState) {
        if (isLegacy() && m_legacyDtrRaised
            && m_socket.state() == QAbstractSocket::ConnectedState) {
            m_socket.write(Spe::Rfc2217::buildSetControl(Spe::Rfc2217::kDtrOff));
            m_socket.write(Spe::Rfc2217::buildSetControl(Spe::Rfc2217::kRtsOff));
            m_socket.waitForBytesWritten(50);
        }
        m_socket.abort();
    }
#ifdef HAVE_SERIALPORT
    if (m_serialPort && m_device == m_serialPort && m_serialPort->isOpen()) {
        // A port in error (adapter unplugged) has no line left to release,
        // and touching it would only raise errorOccurred again.
        if (isLegacy() && m_serialPort->error() == QSerialPort::NoError) {
            m_serialPort->setDataTerminalReady(false);
            m_serialPort->setRequestToSend(false);
        }
        m_serialPort->close();
    }
#endif
    m_device = nullptr;
    m_legacyDtrRaised = false;
}

void SpeConnection::drainWrites()
{
    if (m_device == &m_socket) {
        if (m_socket.state() == QAbstractSocket::ConnectedState) {
            m_socket.waitForBytesWritten(50);
        }
#ifdef HAVE_SERIALPORT
    } else if (m_serialPort && m_device == m_serialPort && m_serialPort->isOpen()) {
        // Qt's Windows close() cancels pending I/O and clears the write
        // buffer, so without this the bytes never leave.
        m_serialPort->waitForBytesWritten(50);
#endif
    }
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
    m_rfc2217NegotiationPending = false;
    m_rfc2217Tail.clear();
    setLcdFresh(false);
    qCInfo(lcTuner) << "SpeConnection: connected via" << description();

    // First poll immediately — the timer only fires after a full interval,
    // and the applet shouldn't sit blank for it. The 1K-FA has no poll and
    // no LCD request: RCU_ON starts its stream, and the timer keeps only
    // silence detection and the RCU_ON retry.
    if (isLegacy()) {
        sendRaw(Spe::Legacy::buildRcuOn());
    } else {
        sendRaw(Spe::buildStatusRequest());
    }
    m_pollTimer.start();
    if (m_lcdWanted && !isLegacy()) {
        applyLcdEffect(m_lcdScheduler.enable());
    }

    emit connected();
}

void SpeConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_pollTimer.stop();
    m_lcdScheduler.reset();
    m_lcdTimer.stop();
    m_lcdStaleTimer.stop();
    setLcdFresh(false);
    m_powerOnTimer.stop();
    m_powerOnStep = -1;
    m_offReleaseTimer.stop();
    m_rfc2217NegotiationPending = false;
    m_parser.reset();
    m_legacyParser.reset();
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

    // Read-only scan for the proxy's answer to WILL COM-PORT-OPTION, so powerOn()
    // knows whether RFC 2217 control is available (the parser resyncs past
    // negotiation itself). Scans the previous chunk's 2-byte tail + this chunk,
    // since the 3-byte DO/DONT can straddle segments; only the tail is carried,
    // so a reply is never counted twice.
    if (m_mode == Mode::Network && m_rfc2217NegotiationPending) {
        const auto reply = Spe::Rfc2217::scanComPortOptionReply(m_rfc2217Tail + chunk);
        m_rfc2217Tail = chunk.right(2);
        if (reply != Spe::Rfc2217::OptionReply::None) {
            m_comPortOption = reply;
            m_rfc2217NegotiationPending = false;
            m_rfc2217Tail.clear();
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

    if (isLegacy()) {
        m_legacyParser.feed(chunk);
    } else {
        m_parser.feed(chunk);
    }
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

    if (isLegacy()) {
        if (m_silentPolls > 0 && m_silentPolls % kLegacyRcuRetryTicks == 0) {
            sendRaw(Spe::Legacy::buildRcuOn());
        }
        return;
    }
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
        // The keys are safe only beside a fresh mirror. Pull the resulting
        // screen immediately when the line is free — and when a display
        // request is already in flight, as pending work the scheduler
        // services the moment that request resolves, never as a second
        // in-flight request (the review-caught overlap path).
        applyLcdEffect(m_lcdScheduler.ackSeen());
        return;
    }

    const auto status = Spe::parseStatus(f.data);
    if (!status) {
        qCWarning(lcTuner) << "SpeConnection: unparseable status reply ("
                            << f.data.size() << "B) —" << f.data.left(24);
        return;
    }
    acceptStatus(*status);
}

void SpeConnection::onLegacyFrameReceived(const Spe::Legacy::Frame& f)
{
    // Raw payload at debug level (Help → Support logging: Tuner/AGM), so a
    // field report can pin the Status offsets to real hardware.
    qCDebug(lcTuner) << "SpeConnection: 1K-FA frame" << f.data.size() << "B:"
                     << f.data.toHex(' ');
    if (f.isReply()) {
        const quint8 code = static_cast<quint8>(f.data.at(0));
        if (code == Spe::Legacy::kAck) {
            qCDebug(lcTuner) << "SpeConnection: 1K-FA ACK";
        } else {
            // NAK = checksum/format error, UNK = command not understood.
            qCWarning(lcTuner) << "SpeConnection: 1K-FA rejected a command, reply"
                               << QString::number(code, 16);
        }
        return;
    }
    if (const auto status = Spe::Legacy::parseStatus(f.data)) {
        acceptStatus(*status);
    }
}

void SpeConnection::acceptStatus(const Spe::Status& status)
{
    m_statusSeenSinceTick = true;
    m_silentPolls = 0;
    if (!m_responding) {
        m_responding = true;
        emit respondingChanged(true);
    }

    m_lastStatus = status;
    if (status.id != m_currentModelId) {
        m_currentModelId = status.id;
        qCInfo(lcTuner) << "SpeConnection: amplifier identifies as" << m_currentModelId
                         << "(" << Spe::modelSpec(m_currentModelId).displayName << ")";
        emit modelChanged(m_currentModelId);
    }
    emit statusUpdated(status);
}

void SpeConnection::sendKey(Spe::Key key)
{
    if (!isLegacy()) {
        sendRaw(Spe::buildKeyCommand(key));
        return;
    }
    sendRaw(Spe::Legacy::buildKeyCommand(key));
    // On the 1K-FA a held-high DTR keeps the amp powered and locks out its
    // own power key, so SWITCH OFF also releases the line — over ser2net via
    // RFC 2217, like powerOn(). The key goes first; the line follows once
    // it is on the wire (m_offReleaseTimer), so the amp is told to shut down
    // rather than having its power switch pulled mid-keystroke.
    if (key == Spe::Key::SwitchOff) {
        m_offReleaseTimer.start();
    }
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
        m_comPortOption = Spe::Rfc2217::OptionReply::None;
        m_rfc2217NegotiationPending = true;
        m_rfc2217Tail.clear();
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
            m_offReleaseTimer.stop();  // a pending OFF release must not undo ON
            setControlLines(true, false);
            if (isLegacy()) {
                // A raw bridge (no RFC 2217 answer) moved no line; a refusing
                // proxy may have (design note §9), so it counts as raised.
                m_legacyDtrRaised = m_mode == Mode::Serial
                    || m_comPortOption != Spe::Rfc2217::OptionReply::None;
                // 1K-FA: DTR held high is the power switch — no pulse. Its
                // boot takes several seconds; the RCU_ON retry in pollTick()
                // picks up the stream once it answers.
                m_powerOnStep = -1;
                m_rfc2217NegotiationPending = false;
                m_rfc2217Tail.clear();
                reportPowerOnOutcome();
                break;
            }
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
            m_rfc2217NegotiationPending = false;
            m_rfc2217Tail.clear();
            reportPowerOnOutcome();
            break;
        default:
            m_powerOnStep = -1;
            m_rfc2217NegotiationPending = false;
            m_rfc2217Tail.clear();
            break;
    }
}

void SpeConnection::reportPowerOnOutcome()
{
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
        emit powerOnReported(PowerOnResult::ProxyRefused);
    } else if (m_mode == Mode::Network
               && m_comPortOption != Spe::Rfc2217::OptionReply::Accepted) {
        // No answer at all: a raw TCP bridge. Besides ser2net in raw
        // mode this covers plain serial-to-Ethernet converters (Waveshare
        // RS232 TO ETH and similar in TCP-server mode), which forward
        // bytes only and offer no way to drive DTR/RTS remotely.
        qCWarning(lcTuner) << "SpeConnection: power-ON pulse sent, but"
                              " the network bridge never answered the"
                              " RFC 2217 COM-port-control request, so"
                              " it cannot drive DTR/RTS — remote"
                              " power-ON is not possible this way."
                              " Plain serial-to-Ethernet converters"
                              " (e.g. Waveshare in TCP-server mode)"
                              " do not support it: power the amplifier"
                              " on at its front panel, or use ser2net"
                              " with `accepter:"
                              " telnet(rfc2217=true),<port>` and a"
                              " serial adapter that has a DTR line.";
        emit powerOnReported(PowerOnResult::NoLineControl);
    } else {
        qCInfo(lcTuner) << "SpeConnection: power-ON pulse complete — the"
                           " amp should begin answering status polls"
                           " shortly.";
        emit powerOnReported(PowerOnResult::Sent);
    }
}

void SpeConnection::sendRaw(const QByteArray& packet)
{
    if (!m_device || !m_connected) { return; }
    m_device->write(packet);
}

}  // namespace AetherSDR
