#include "AcomConnection.h"
#include "LogManager.h"

namespace AetherSDR {

AcomConnection::AcomConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &AcomConnection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &AcomConnection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &AcomConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    m_parser.setFrameCallback([this](const Acom::Frame& f) { onFrameReceived(f); });

    // Retries every 5s indefinitely until the amp returns or the user
    // disconnects — matches PgxlConnection/TgxlConnection's precedent for a
    // peripheral that may be power-cycling or, on the serial side, unplugged.
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

    m_keepaliveTimer.setInterval(200);
    connect(&m_keepaliveTimer, &QTimer::timeout, this, [this]() {
        if (!m_frameSeenSinceTick) {
            sendRaw(Acom::buildTelemetryEnable());
        }
        m_frameSeenSinceTick = false;
    });

    // Model auto-detection request/retry (design doc §6). 800ms is generous
    // relative to the protocol's own 10ms inter-message pacing — this is a
    // one-shot query at connect time, not a latency-sensitive path, so
    // there's no reason to race it.
    m_systemConfigRetryTimer.setSingleShot(true);
    m_systemConfigRetryTimer.setInterval(800);
    connect(&m_systemConfigRetryTimer, &QTimer::timeout, this, &AcomConnection::sendSystemConfigRequest);
}

QString AcomConnection::description() const
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

QString AcomConnection::sourceLabel() const
{
    switch (m_mode) {
        case Mode::Network: return QStringLiteral("NETWORK");
        case Mode::Serial:  return QStringLiteral("SERIAL");
        default:            return QStringLiteral("—");
    }
}

#ifdef HAVE_SERIALPORT
void AcomConnection::connectSerial(const QString& portName)
{
    m_mode = Mode::Serial;
    m_lastSerialPort = portName;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    if (!m_serialPort) {
        m_serialPort = new QSerialPort(this);
        connect(m_serialPort, &QSerialPort::readyRead, this, &AcomConnection::onReadyRead);
        connect(m_serialPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError err) {
            if (err == QSerialPort::NoError) { return; }
            const QString msg = m_serialPort->errorString();
            qCWarning(lcTuner) << "AcomConnection: serial error" << err << msg;
            onTransportError(msg);
            if (m_connected) { onTransportDown(); }
        });
    }

    m_serialPort->setPortName(portName);
    // Fixed by the amplifier's own protocol spec — not user-configurable.
    m_serialPort->setBaudRate(QSerialPort::Baud9600);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        const QString err = m_serialPort->errorString();
        qCWarning(lcTuner) << "AcomConnection: failed to open" << portName << err;
        emit connectionFailed(err);
        if (m_autoReconnect) { armReconnect(); }
        return;
    }
    // Hold DTR/RTS low — avoids interfering with the amplifier's own
    // front-panel power-button logic (same precaution the open-source
    // reference client takes; see THIRD_PARTY_LICENSES).
    m_serialPort->setDataTerminalReady(false);
    m_serialPort->setRequestToSend(false);

    m_device = m_serialPort;
    onTransportUp();
}
#endif

void AcomConnection::connectNetwork(const QString& host, quint16 port)
{
    m_mode = Mode::Network;
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    m_device = &m_socket;
    qCDebug(lcTuner) << "AcomConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
}

void AcomConnection::disconnect()
{
    // Self-contained: don't depend on teardownDevice() indirectly triggering
    // onTransportDown() to do this cleanup. It sometimes does (QTcpSocket::
    // abort() synchronously emits disconnected()) and sometimes doesn't
    // (QSerialPort::close() emits nothing at all) — relying on that meant a
    // user-initiated disconnect silently never fired AcomConnection's own
    // disconnected() signal on either transport, leaving the applet stuck
    // showing "Connected" (its only disconnected() handler drives
    // setAcomVisible(false)).
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_keepaliveTimer.stop();
    m_systemConfigRetryTimer.stop();
    m_connected = false;
    teardownDevice();
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "AcomConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void AcomConnection::teardownDevice()
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

void AcomConnection::onTransportUp()
{
    m_connected = true;
    m_frameSeenSinceTick = false;
    m_telemetryDecodeFailed = false;
    m_errorCodesDecodeFailed = false;
    m_pendingAutoRangeTier.clear();
    m_autoRangeStreak = 0;
    qCInfo(lcTuner) << "AcomConnection: connected via" << description();
    sendRaw(Acom::buildTelemetryEnable());
    m_keepaliveTimer.start();

    // Reset to the known-good starting tier on every fresh connect — never
    // carries a prior session's auto-scaled tier forward (design doc §6).
    m_currentModel = QStringLiteral("600S");
    emit modelChanged(m_currentModel, QStringLiteral("default"));
    requestSystemConfig();

    emit connected();
}

void AcomConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_keepaliveTimer.stop();
    m_systemConfigRetryTimer.stop();
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "AcomConnection: disconnected";
        emit disconnected();
    }
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void AcomConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "AcomConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
        armReconnect();
    }
}

void AcomConnection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void AcomConnection::onReadyRead()
{
    if (!m_device) { return; }
    m_parser.feed(m_device->readAll());
}

void AcomConnection::onFrameReceived(const Acom::Frame& f)
{
    m_frameSeenSinceTick = true;

    switch (static_cast<Acom::Address>(f.address)) {
        case Acom::Address::Telemetry: {
            // No 0x86 ack here — Telemetry is auto-pushed continuously (every
            // ~100ms in practice), not requested. The spec's per-message
            // resend-until-acked mandate (§1/§5.5) reads as written for a
            // request/reply exchange; the MIT-licensed reference client (see
            // THIRD_PARTY_LICENSES) never sends 0x86 for anything, on any
            // message type, and is a documented-working real-hardware
            // implementation over direct RS-232 — so acking isn't required to
            // keep the push stream flowing, and continuously acking a
            // ~10/s broadcast is the one place blanket-acking every frame
            // type stopped making sense.
            auto t = Acom::decodeTelemetry(f.data);
            if (t) {
                if (m_telemetryDecodeFailed) {
                    qCInfo(lcTuner) << "AcomConnection: telemetry decode recovered"
                                       " (" << f.data.size() << "B).";
                    m_telemetryDecodeFailed = false;
                }
                m_lastTelemetry = *t;
                emit telemetryUpdated(*t);
                maybeAutoRangeUp(*t);
            } else if (!m_telemetryDecodeFailed) {
                // Checksum-valid frame the decoder rejected as too short. Without
                // this the ACOM panel just stays blank — connected, frames
                // arriving, gauges dead, nothing logged. Likely a firmware
                // variant with a shorter telemetry frame, or link corruption.
                // Latched (see m_telemetryDecodeFailed) so a permanently-short
                // stream logs once, not at the ~10 Hz frame rate.
                m_telemetryDecodeFailed = true;
                qCWarning(lcTuner) << "AcomConnection: telemetry frame too short to"
                                      " decode (" << f.data.size() << "B) — gauges will"
                                      " stay blank. Firmware variant, or a corrupt link?";
            }
            break;
        }
        case Acom::Address::ErrorCodes:
            // Also auto-pushed, not requested — same reasoning as Telemetry.
            if (f.data.size() >= 20) {
                if (m_errorCodesDecodeFailed) {
                    m_errorCodesDecodeFailed = false;
                }
                emit rawErrorCodesUpdated(f.data);
            } else if (!m_errorCodesDecodeFailed) {
                m_errorCodesDecodeFailed = true;
                qCWarning(lcTuner) << "AcomConnection: error-codes frame too short"
                                      " (" << f.data.size() << "B, need >= 20) — fault"
                                      " state won't update. Firmware variant?";
            }
            break;
        case Acom::Address::SystemConfig: {
            // SystemConfig, unlike Telemetry/ErrorCodes, is a genuine one-shot
            // request/reply (see buildRequestMessage/sendSystemConfigRequest's
            // own retry-with-timeout) — acking it matches the spec's
            // resend-until-acked model for that exchange shape.
            sendRaw(Acom::buildAck(f.address));
            m_systemConfigRetryTimer.stop();
            auto cfg = Acom::decodeSystemConfig(f.data);
            if (cfg) {
                qCInfo(lcTuner) << "AcomConnection: SystemConfig — amplifierType="
                                 << cfg->amplifierType << "fw=" << cfg->fwVersion << "."
                                 << cfg->fwSubVersion << "serial=" << cfg->serialNumberHex
                                 << "hardFaults=" << cfg->hardFaultCount;
                emit systemConfigReceived(*cfg);

                const QString confirmed = Acom::modelNameForAmplifierType(cfg->amplifierType);
                if (confirmed.isEmpty()) {
                    qCInfo(lcTuner) << "AcomConnection: unrecognized amplifierType"
                                     << cfg->amplifierType
                                     << "— no confirmed mapping yet; relying on auto-ranging."
                                        " Please report this value if you know your model"
                                        " (see docs/architecture/acom-600s-amplifier-design.md).";
                    break;
                }
                // Same up-only guard as maybeAutoRangeUp(): a genuine but
                // delayed SystemConfig reply (the request/retry sequence can
                // take up to ~4s) must never snap the tier back down below
                // whatever auto-ranging has already confirmed from observed
                // power in the meantime — see design doc §6.3 point 4 and
                // this class's own currentModel() doc comment.
                const auto& order = Acom::orderedModelTiers();
                if (order.indexOf(confirmed) < order.indexOf(m_currentModel)) {
                    qCInfo(lcTuner) << "AcomConnection: SystemConfig confirms" << confirmed
                                     << "but auto-ranging already established" << m_currentModel
                                     << "from observed power — keeping the higher tier.";
                    break;
                }
                // Either a genuine change (including the common case:
                // default "600S" -> confirmed "600S", same name but the GUI
                // still wants the reason to change for its diagnostic
                // tooltip) or confirming the tier auto-ranging already
                // reached — either way it's now "confirmed", not "default"
                // or "auto-scaled".
                m_currentModel = confirmed;
                emit modelChanged(m_currentModel, QStringLiteral("confirmed"));
            }
            break;
        }
        default:
            break;
    }
}

void AcomConnection::requestSystemConfig()
{
    m_systemConfigAttempts = 0;
    sendSystemConfigRequest();
}

void AcomConnection::sendSystemConfigRequest()
{
    if (m_systemConfigAttempts >= kSystemConfigMaxAttempts) {
        qCInfo(lcTuner) << "AcomConnection: gave up requesting SystemConfig after"
                         << kSystemConfigMaxAttempts << "attempts — amp may not support"
                            " this query on its firmware; auto-ranging still applies.";
        return;
    }
    ++m_systemConfigAttempts;
    qCDebug(lcTuner) << "AcomConnection: requesting SystemConfig, attempt"
                      << m_systemConfigAttempts << "of" << kSystemConfigMaxAttempts;
    sendRaw(Acom::buildRequestMessage(static_cast<quint8>(Acom::Address::SystemConfig)));
    m_systemConfigRetryTimer.start();
}

void AcomConnection::maybeAutoRangeUp(const Acom::Telemetry& t)
{
    const QString required = Acom::tierForForwardPower(static_cast<float>(t.forwardPowerW));
    const auto& order = Acom::orderedModelTiers();
    if (order.indexOf(required) <= order.indexOf(m_currentModel)) {
        // At/above the required tier already — reset any in-progress streak so a
        // lone spike followed by normal readings doesn't accumulate across gaps.
        m_autoRangeStreak = 0;
        m_pendingAutoRangeTier.clear();
        return;  // never downgrades
    }

    // Debounce before ratcheting: the tier only ever moves UP and never back
    // down within a session, so acting on a single frame means one corrupt-
    // but-checksum-valid reading (the 8-bit checksum passes ~1/256 of corrupted
    // frames) over-ranges every gauge irreversibly until reconnect. Require the
    // SAME higher tier on kAutoRangeConsecutiveFrames consecutive frames — a
    // spurious spike won't repeat; a genuinely bigger amp will.
    if (required != m_pendingAutoRangeTier) {
        m_pendingAutoRangeTier = required;
        m_autoRangeStreak = 1;
        return;
    }
    if (++m_autoRangeStreak < kAutoRangeConsecutiveFrames) {
        return;
    }

    qCInfo(lcTuner) << "AcomConnection: observed" << t.forwardPowerW
                     << "W forward power exceeds" << m_currentModel << "tier ceiling on"
                     << m_autoRangeStreak << "consecutive frames — auto-scaling up to"
                     << required;
    m_currentModel = required;
    m_autoRangeStreak = 0;
    m_pendingAutoRangeTier.clear();
    emit modelChanged(m_currentModel, QStringLiteral("auto-scaled"));
}

void AcomConnection::sendRaw(const QByteArray& frame)
{
    if (!m_device || !m_connected) { return; }
    m_device->write(frame);
}

void AcomConnection::setOperate(bool on)
{
    sendRaw(Acom::buildModeCommand(on ? Acom::ModeCommand::Operate : Acom::ModeCommand::Standby));
}

void AcomConnection::powerOff()
{
    sendRaw(Acom::buildModeCommand(Acom::ModeCommand::PowerOff));
}

void AcomConnection::clearFaults()
{
    sendRaw(Acom::buildClearFaultsCommand());
}

}  // namespace AetherSDR
