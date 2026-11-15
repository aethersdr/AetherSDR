#include "Kpa500Connection.h"
#include "Kpa500Maintenance.h"

#include <algorithm>

#include <QChar>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcKpa500, "aethersdr.kpa500")

namespace AetherSDR {

Kpa500Connection::Kpa500Connection(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    m_maintenanceTimer.setSingleShot(true);
    m_maintenanceTimer.setInterval(kMaintenanceTimeoutMs);
    connect(&m_maintenanceTimer, &QTimer::timeout, this, &Kpa500Connection::onMaintenanceTimeout);
    m_parser.setErrorCallback([this](const QString& error) { report("REJECT " + error); });
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Kpa500Connection::retryConnection);
    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(kConnectTimeoutMs);
    connect(&m_connectTimer, &QTimer::timeout, this, &Kpa500Connection::onConnectTimeout);

    m_socket.setReadBufferSize(4096);
    connect(&m_socket, &QTcpSocket::connected, this, [this] {
        if (m_network && m_connecting && !m_deliberateDisconnect) {
            onTransportUp();
        }
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_network && (m_connected || m_connecting)) {
            onTransportDown(tr("TCP connection closed"));
        }
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, &Kpa500Connection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_network && (m_connected || m_connecting)) {
            onTransportError(m_socket.errorString());
        }
    });

    m_fastPollTimer.setInterval(kFastPollMs);
    connect(&m_fastPollTimer, &QTimer::timeout, this, &Kpa500Connection::fastPollTick);

    m_slowPollTimer.setInterval(kSlowPollMs);
    connect(&m_slowPollTimer, &QTimer::timeout, this, &Kpa500Connection::slowPollTick);

    m_parser.setCallback([this](const QString& cmd, const QString& arg) {
        if (!m_connected) {
            return;
        }
        if (cmd == "ER" || cmd == "EW") {
            handleMaintenanceReply(cmd, arg);
            return;
        }
        bool accepted = false;
        Kpa500::Status next = m_status;
        bool changed = Kpa500::applyMessage(cmd, arg, next, &accepted);
        if (!accepted) {
            report(QStringLiteral("REJECT ^%1%2; malformed or unsupported reply").arg(cmd, arg));
            return;
        }
        const bool fieldChanged = changed;
        // Only a power-status response to a fresh probe can leave quiet mode.
        // Replies already in flight when OFF was requested cannot revive polling.
        if (m_powerCheckOnly && (cmd != "ON" || (next.powerOn == true && !m_powerCheckPending))) {
            return;
        }
        if (cmd == "ON") {
            if (next.powerOn == false) {
                const QString firmware = next.firmwareVersion.value_or(QString());
                const QString serial = next.serialNumber.value_or(QString());
                next = {};
                next.powerOn = false;
                if (!firmware.isEmpty()) { next.firmwareVersion = firmware; }
                if (!serial.isEmpty()) { next.serialNumber = serial; }
                m_received.clear();
                changed = true;
                enterPowerCheckMode();
            } else if (m_powerCheckOnly) {
                m_powerCheckOnly = false;
                m_powerOffRequested = false;
                m_connectCmdIndex = m_fastIndex = m_slowIndex = 0;
                m_connectCmdsDone = false;
                report("POWER ON confirmed; normal telemetry polling resumed");
            }
            m_powerCheckPending = false;
        }
        m_status = next;
        m_received[cmd] = m_clock.elapsed();
        if (!m_detected) {
            m_detected = true;
            emit detectedChanged(true);
        }
        QString decoded = QStringLiteral("DECODE %1 = %2").arg(cmd, arg);
        if (cmd == "WS") {
            decoded += QStringLiteral(" | %1 W; SWR %2").arg(m_status.forwardPowerW.value_or(0))
                .arg(m_status.swr ? QString::number(*m_status.swr, 'f', 1) : QStringLiteral("unmeasured"));
        } else if (cmd == "VI") {
            decoded += QStringLiteral(" | %1 V; %2 A").arg(m_status.paVoltageV.value_or(0), 0, 'f', 1)
                .arg(m_status.paCurrentA.value_or(0), 0, 'f', 1);
        } else if (cmd == "BN") {
            decoded += " | " + Kpa500::bandName(m_status.band.value_or(-1));
        }
        report(decoded);
        if (fieldChanged && (cmd == "ON" || cmd == "OS" || cmd == "BN" || cmd == "FL" || cmd == "FC" || cmd == "SP")) {
            report(QStringLiteral("STATE %1 = %2").arg(cmd, arg));
        }
        emit telemetryReceived(cmd, m_status);
        m_statusSeenSinceFastTick = true;
        if (!m_responding) {
            m_silentTicks = 0;
            // The port may have stayed open across an amplifier power cycle.
            // Refresh identity after replies resume, as on a new connection.
            if (m_connectCmdsDone) {
                m_connectCmdIndex = 0;
                m_connectCmdsDone = false;
            }
            setResponding(true);
        }
        if (changed) {
            emit statusUpdated(m_status);
        }
        handleMaintenanceStatus(cmd);
        updateBand();
    });
}

Kpa500Connection::~Kpa500Connection()
{
    // Members close after the state/timers are destroyed; do not run callbacks then.
    m_socket.disconnect(this);
#ifdef HAVE_SERIALPORT
    m_serial.disconnect(this);
#endif
}

QString Kpa500Connection::description() const
{
    if (m_network && !m_lastHost.isEmpty()) {
        const QString host = m_lastHost.contains(':') ? '[' + m_lastHost + ']' : m_lastHost;
        return tr("Raw TCP %1:%2 (baud set on server)").arg(host).arg(m_lastTcpPort);
    }
    if (m_lastPortName.isEmpty()) {
        return {};
    }
    return QStringLiteral("%1 @ %2").arg(m_lastPortName).arg(m_lastBaudRate);
}

void Kpa500Connection::connectToHost(const QString& host, quint16 port)
{
    if (m_connected || m_connecting) {
        return;
    }
    m_reconnectTimer.stop();
    const QString trimmedHost = host.trimmed();
    bool invalidHost = trimmedHost.isEmpty() || trimmedHost.size() > 253 || trimmedHost.contains('/');
    for (QChar character : trimmedHost) {
        invalidHost = invalidHost || character.isSpace() || !character.isPrint();
    }
    if (invalidHost || port == 0) {
        emit connectionFailed(tr("Enter a TCP host and a port from 1 to 65535."));
        return;
    }
    m_network = true;
    m_lastHost = trimmedHost;
    m_lastTcpPort = port;
    m_deliberateDisconnect = false;
    m_connecting = true;
    m_device = &m_socket;
    m_parser.reset();
    report("CONNECTING " + description());
    m_connectTimer.start();
    m_socket.connectToHost(m_lastHost, m_lastTcpPort);
}

void Kpa500Connection::onConnectTimeout()
{
    if (m_connecting) {
        onTransportError(tr("TCP connection timed out after 10 seconds."));
    }
}

void Kpa500Connection::retryConnection()
{
    if (!m_autoReconnect || m_deliberateDisconnect || m_connected || m_connecting) {
        return;
    }
    if (m_network && !m_lastHost.isEmpty()) {
        connectToHost(m_lastHost, m_lastTcpPort);
#ifdef HAVE_SERIALPORT
    } else if (!m_network && !m_lastPortName.isEmpty()) {
        connectSerial(m_lastPortName, m_lastBaudRate);
#endif
    }
}

void Kpa500Connection::onReadyRead()
{
    if (m_connected && m_device) {
        receiveBytes(m_device->readAll());
    }
}

#ifdef HAVE_SERIALPORT

void Kpa500Connection::connectSerial(const QString& portName, int baudRate)
{
    if (m_connected || m_connecting) {
        return;
    }
    m_reconnectTimer.stop();
    m_network = false;

    m_lastPortName  = portName;
    m_lastBaudRate  = baudRate;
    m_deliberateDisconnect = false;

    qCDebug(lcKpa500) << "Kpa500Connection: opening" << portName << "@" << baudRate;

    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial.open(QIODevice::ReadWrite)) {
        const QString err = m_serial.errorString();
        qCWarning(lcKpa500) << "Kpa500Connection: open failed:" << err;
        emit connectionFailed(err);
        armReconnect();
        return;
    }

    m_device = &m_serial;
    onTransportUp();
}

void Kpa500Connection::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;
    if (!m_connected)
        return;
    qCWarning(lcKpa500) << "Kpa500Connection: serial error:" << error << m_serial.errorString();
    onTransportError(m_serial.errorString());
}

#endif  // HAVE_SERIALPORT

void Kpa500Connection::disconnect()
{
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    onTransportDown();
}

void Kpa500Connection::setAutoReconnect(bool on)
{
    m_autoReconnect = on;
    if (!on) {
        m_reconnectTimer.stop();
    }
}

void Kpa500Connection::setOperate(bool on)
{
    if (maintenanceBusy() || !m_responding || m_powerCheckOnly || m_powerOffRequested || busy()) { return; }
    sendRaw(on ? Kpa500::buildOperate() : Kpa500::buildStandby());
    sendRaw(Kpa500::buildQuery("OS"));
}

void Kpa500Connection::clearFault()
{
    if (maintenanceBusy() || !m_responding || m_powerCheckOnly || m_powerOffRequested || busy()) { return; }
    sendRaw(Kpa500::buildClearFault());
    sendRaw(Kpa500::buildQuery("FL"));
}

void Kpa500Connection::setFanSpeed(int n)
{
    if (maintenanceBusy() || !m_responding || m_powerCheckOnly || m_powerOffRequested || n < 0 || n > 6) { return; }
    sendRaw(Kpa500::buildSetFanSpeed(n));
    sendRaw(Kpa500::buildQuery("FC"));
}

// ── Private ───────────────────────────────────────────────────────────────────

void Kpa500Connection::onTransportUp()
{
    m_connectTimer.stop();
    m_reconnectTimer.stop();
    m_connecting = false;
    m_connected = true;
    m_status    = {};
    m_received.clear();
    m_powerOffRequested = false;
    m_powerCheckOnly = false;
    m_powerCheckPending = false;
    m_wakeTicks = 0;
    m_sentBand = m_failedBand = m_manualBand = -1;
    report("CONNECTED " + description());
    m_parser.reset();
    m_slowIndex       = 0;
    m_fastIndex       = 0;
    m_connectCmdIndex = 0;
    m_connectCmdsDone = false;
    m_silentTicks     = 0;
    m_statusSeenSinceFastTick = false;

#ifdef HAVE_SERIALPORT
    if (!m_network) {
        connect(&m_serial, &QSerialPort::readyRead,
                this, &Kpa500Connection::onReadyRead, Qt::UniqueConnection);
        connect(&m_serial, &QSerialPort::errorOccurred,
                this, &Kpa500Connection::onSerialError, Qt::UniqueConnection);
    }
#endif

    // Send the protocol null probe before polling; only a valid status reply
    // establishes liveness, since the parser discards the bare echo.
    sendRaw(Kpa500::buildNullCommand());
    if (!m_connected) {
        return;
    }
    enterPowerCheckMode();
    m_powerCheckPending = true;
    sendRaw(Kpa500::buildQuery("ON"));
    if (!m_connected) {
        return;
    }

    m_fastPollTimer.start();
    m_slowPollTimer.start();

    qCDebug(lcKpa500) << "Kpa500Connection: connected to" << description();
    emit connected();
}

void Kpa500Connection::onTransportDown(const QString& reason)
{
    const bool wasActive = m_connected || m_connecting;
    m_connecting = false;
    m_connected = false;
    if (maintenanceBusy()) {
        failMaintenance(tr("Amplifier connection closed during maintenance."));
    }
    m_connectTimer.stop();
    m_fastPollTimer.stop();
    m_slowPollTimer.stop();
    m_device = nullptr;
    m_socket.abort();
#ifdef HAVE_SERIALPORT
    m_serial.disconnect(this);
    if (m_serial.isOpen()) {
        m_serial.close();
    }
#endif
    m_parser.reset();
    setResponding(false);
    m_status = {};
    m_received.clear();
    m_powerCheckPending = false;
    m_sentBand = m_failedBand = m_manualBand = -1;
    if (m_detected) {
        m_detected = false;
        emit detectedChanged(false);
    }
    if (wasActive) {
        report("DISCONNECTED " + reason);
        updateBand();
        emit disconnected();
        armReconnect();
    }
}

void Kpa500Connection::onTransportError(const QString& reason)
{
    if (!m_connected && !m_connecting) {
        return;
    }
    onTransportDown(reason);
    qCWarning(lcKpa500) << "Kpa500Connection: transport error:" << reason;
    emit connectionFailed(reason);
}

void Kpa500Connection::fastPollTick()
{
    if (!m_connected || maintenanceBusy()) {
        return;
    }

    if (m_wakeTicks > 0) { --m_wakeTicks; return; }
    expireTelemetry();
    updateBand();
    // Quiet power checks are five seconds apart, so an explicit OFF reply
    // stays live for eight seconds. Silence alone still never means OFF.
    if (m_statusSeenSinceFastTick) {
        m_silentTicks = 0;
        m_statusSeenSinceFastTick = false;
    } else {
        const int limit = m_powerCheckOnly && m_status.powerOn == false ? 40 : kSilentTickLimit;
        if (m_silentTicks < limit) { ++m_silentTicks; }
        if (m_silentTicks >= limit) {
            setResponding(false);
            enterPowerCheckMode();
        }
    }
    if (m_powerCheckOnly) {
        if (m_clock.elapsed() - m_lastPowerCheckAt >= kPowerCheckMs) {
            m_lastPowerCheckAt = m_clock.elapsed();
            m_powerCheckPending = true;
            sendRaw(Kpa500::buildQuery("ON"));
        }
        return;
    }
    if (!m_connectCmdsDone) {
        const QStringList& cmds = Kpa500::connectCommands();
        if (m_connectCmdIndex < cmds.size()) {
            sendRaw(Kpa500::buildQuery(cmds[m_connectCmdIndex].toLatin1().constData()));
            ++m_connectCmdIndex;
            return;
        }
        m_connectCmdsDone = true;
    }

    // Rotate through fast-poll commands.
    const QStringList& cmds = Kpa500::fastPollCommands();
    if (!cmds.isEmpty()) {
        sendRaw(Kpa500::buildQuery(cmds[m_fastIndex].toLatin1().constData()));
        m_fastIndex = (m_fastIndex + 1) % cmds.size();
    }
}

void Kpa500Connection::slowPollTick()
{
    if (maintenanceBusy() || !m_connected || m_powerCheckOnly || !m_connectCmdsDone || m_wakeTicks > 0)
        return;

    const QStringList& cmds = Kpa500::slowPollCommands();
    if (cmds.isEmpty())
        return;

    const QString& cmd = cmds[m_slowIndex];
    // A bootloader P wakes the amp, including the P inside ^SP;.
    if (cmd != "SP" || (m_responding && m_status.powerOn.value_or(false) && !m_powerOffRequested)) {
        sendRaw(Kpa500::buildQuery(cmd.toLatin1().constData()));
    }
    m_slowIndex = (m_slowIndex + 1) % cmds.size();
}

void Kpa500Connection::sendRaw(const QByteArray& bytes)
{
    if (!m_connected || !m_device || bytes.isEmpty()) {
        return;
    }
    // Stop a stalled bridge before queued polls or control requests grow unbounded.
    if (m_device->bytesToWrite() + bytes.size() > kMaxPendingWriteBytes) {
        onTransportError(tr("Transport write queue is full; connection closed."));
        return;
    }
    if (m_device->write(bytes) != bytes.size()) {
        onTransportError(tr("Transport write failed; connection closed."));
        return;
    }
    report("TX " + QString::fromLatin1(bytes.toPercentEncoding("^; ")));
}

void Kpa500Connection::armReconnect()
{
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected && !m_connecting
        && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void Kpa500Connection::setResponding(bool responding)
{
    if (m_responding == responding) {
        return;
    }
    m_responding = responding;
    if (!responding && maintenanceBusy()) {
        failMaintenance(tr("Amplifier stopped responding during maintenance."));
    }
    if (!responding) {
        m_status = {};
        m_received.clear();
        m_sentBand = m_failedBand = m_manualBand = -1;
        m_parser.reset();
        m_statusSeenSinceFastTick = false;
    }
    report(responding ? "REPLIES resumed" : "STALE no recent valid reply; power state unknown");
    emit respondingChanged(responding);
    updateBand();
    if (!responding) {
        emit statusUpdated(m_status);
    }
}

}  // namespace AetherSDR

namespace AetherSDR {
void Kpa500Connection::report(const QString& line)
{
    qCDebug(lcKpa500).noquote() << line;
    emit diagnostic(line);
}

void Kpa500Connection::receiveBytes(const QByteArray& bytes)
{
    report("RX " + QString::fromLatin1(bytes.toPercentEncoding("^; ")));
    m_parser.feed(bytes);
}

bool Kpa500Connection::busy() const
{
    return m_transmitting || m_status.forwardPowerW.value_or(0.0f) > 0.0f;
}

void Kpa500Connection::setPower(bool on)
{
    if (maintenanceBusy() || !m_connected || busy()) { report("POWER blocked: maintenance, disconnected or transmitting"); return; }
    m_powerOffRequested = !on;
    m_sentBand = m_failedBand = m_manualBand = -1;
    enterPowerCheckMode();
    m_powerCheckPending = false;
    m_lastPowerCheckAt = m_clock.elapsed();
    if (on) {
        sendRaw(Kpa500::buildPowerOn());
        m_lastPowerCheckAt -= kPowerCheckMs;
        m_wakeTicks = 10; // Give boot checks two seconds without poll traffic.
        report("POWER ON requested; waiting for amplifier readback");
    } else {
        sendRaw(Kpa500::buildPowerOff());
        report("POWER OFF requested; silence alone cannot confirm power state");
    }
    updateBand();
}

void Kpa500Connection::setSpeaker(bool on)
{
    if (maintenanceBusy() || !m_responding || m_powerCheckOnly || !m_status.powerOn.value_or(false) || m_powerOffRequested) { return; }
    sendRaw(Kpa500::buildSetSpeaker(on));
    sendRaw(Kpa500::buildQuery("SP"));
}

void Kpa500Connection::setFollowTx(bool on)
{
    if (m_followTx != on) {
        m_followTx = on;
        m_manualBand = m_failedBand = -1;
    }
    updateBand();
}

void Kpa500Connection::setTransmitting(bool transmitting)
{
    const bool transmittingChanged = m_transmitting != transmitting;
    m_transmitting = transmitting;
    if (transmittingChanged) { emit maintenanceAvailabilityChanged(); }
    if (transmitting && maintenanceBusy()) {
        failMaintenance(tr("Amplifier maintenance stopped because the radio is transmitting."));
    }
    updateBand();
}

void Kpa500Connection::setTxContext(int band, const QString& source, bool transmitting)
{
    if (band != m_txBand || source != m_txSource) { m_failedBand = -1; }
    m_txBand = band >= 0 && band <= 10 ? band : -1;
    m_txSource = source;
    const bool transmittingChanged = m_transmitting != transmitting;
    m_transmitting = transmitting;
    if (transmittingChanged) { emit maintenanceAvailabilityChanged(); }
    if (transmitting && maintenanceBusy()) {
        failMaintenance(tr("Amplifier maintenance stopped because the radio is transmitting."));
    }
    updateBand();
}

void Kpa500Connection::requestBand(int band)
{
    if (maintenanceBusy() || m_followTx || band < 0 || band > 10 || !m_responding || m_powerCheckOnly) { return; }
    m_manualBand = band;
    m_failedBand = -1;
    updateBand();
}

QString Kpa500Connection::bandTrackingText() const { return m_trackingText; }

void Kpa500Connection::updateBand()
{
    if (maintenanceBusy()) {
        return;
    }
    const int target = m_followTx ? m_txBand : m_manualBand;
    QString text = m_followTx ? QStringLiteral("Following TX %1").arg(m_txSource) : QStringLiteral("Manual band");
    if (m_sentBand >= 0 && m_status.band == m_sentBand) {
        report("BAND confirmed " + Kpa500::bandName(m_sentBand));
        m_sentBand = -1;
    }
    if (m_sentBand >= 0 && m_clock.elapsed() - m_bandSentAt > 3000) {
        m_failedBand = m_sentBand;
        m_sentBand = -1;
        report("BAND readback timed out; no automatic retry");
    }
    if (target < 0) {
        if (m_followTx) { text += " — no supported TX band"; }
    } else {
        text += " → " + Kpa500::bandName(target);
        if (!m_connected || !m_responding || !m_status.powerOn.value_or(false)
            || !m_status.band || !m_status.forwardPowerW || m_powerCheckOnly || m_powerOffRequested || m_wakeTicks > 0) {
            text += " (waiting for amplifier)";
        } else if (m_status.band == target && m_sentBand < 0) {
            if (!m_followTx) { m_manualBand = -1; }
        } else if (busy()) {
            text += " (queued until RX)";
        } else if (m_failedBand == target) {
            text += " (not confirmed; select again or toggle Follow TX)";
        } else if (m_sentBand >= 0) {
            text += " (waiting for band readback)";
        } else {
            m_sentBand = target;
            m_bandSentAt = m_clock.elapsed();
            sendRaw(Kpa500::buildSetBand(target));
            sendRaw(Kpa500::buildQuery("BN"));
            text += " (waiting for band readback)";
        }
    }
    if (text != m_trackingText) {
        m_trackingText = text;
        report(text);
        emit bandTrackingChanged(text);
    }
}

QString Kpa500Connection::maintenanceUnavailableReason(bool writing) const
{
    if (maintenanceBusy()) {
        return tr("Another amplifier maintenance operation is running.");
    }
    if (!m_connected || !m_responding || m_powerCheckOnly || m_powerOffRequested || m_wakeTicks > 0) {
        return tr("Wait for a responding, powered-on amplifier.");
    }
    if (m_status.powerOn != true) {
        return tr("The amplifier must be powered on.");
    }
    if (m_status.firmwareVersion != QStringLiteral("01.54")) {
        return tr("Maintenance is supported only with confirmed KPA500 firmware 01.54.");
    }
    if (!m_status.serialNumber || m_status.serialNumber->isEmpty()) {
        return tr("Wait for the amplifier serial number.");
    }
    if (m_transmitting || m_status.forwardPowerW.value_or(1.0f) != 0.0f) {
        return tr("Stop radio transmission and wait for confirmed zero amplifier power.");
    }
    if (writing && m_status.operate != false) {
        return tr("Put the amplifier in Standby before changing its configuration.");
    }
    if (m_sentBand >= 0) {
        return tr("Wait for the pending amplifier band change to finish.");
    }
    return {};
}

bool Kpa500Connection::maintenanceWriting() const
{
    return m_maintenanceOperation == MaintenanceOperation::BannerWrite
        || m_maintenanceOperation == MaintenanceOperation::ConfigurationRestore;
}

bool Kpa500Connection::beginMaintenance(MaintenanceOperation operation, int startAddress,
                                       int length, const QByteArray& writeData)
{
    const bool writing = operation == MaintenanceOperation::BannerWrite
        || operation == MaintenanceOperation::ConfigurationRestore;
    const QString reason = maintenanceUnavailableReason(writing);
    if (!reason.isEmpty()) {
        emit maintenanceFailed(reason);
        return false;
    }
    m_maintenanceOperation = operation;
    m_maintenanceStartAddress = startAddress;
    m_maintenanceLength = length;
    m_maintenanceOffset = 0;
    m_maintenanceExpectedLength = 0;
    m_maintenanceData.clear();
    m_maintenanceWriteData = writeData;
    m_maintenanceWriteStarted = false;
    m_maintenanceSerial = *m_status.serialNumber;
    m_maintenanceFirmware = *m_status.firmwareVersion;
    m_fastPollTimer.stop();
    m_slowPollTimer.stop();
    m_sentBand = m_failedBand = m_manualBand = -1;
    report("MAINTENANCE started; normal polling and controls paused");
    emit maintenanceBusyChanged(true);
    if (!maintenanceBusy()) {
        return false;
    }
    emit maintenanceProgress(0, writing ? 2 * length : length);
    if (!maintenanceBusy()) {
        return false;
    }
    if (writing) {
        startMaintenancePreflight();
    } else {
        m_maintenancePhase = MaintenancePhase::Reading;
        sendMaintenanceBlock();
    }
    return maintenanceBusy();
}

void Kpa500Connection::readConfiguration()
{
    beginMaintenance(MaintenanceOperation::ConfigurationRead, 0, 224);
}

void Kpa500Connection::readFaultHistory()
{
    beginMaintenance(MaintenanceOperation::FaultHistoryRead, 0, 1024);
}

void Kpa500Connection::readPowerOnBanner()
{
    beginMaintenance(MaintenanceOperation::BannerRead, 0x50, 8);
}

void Kpa500Connection::writePowerOnBanner(const QString& banner)
{
    bool valid = banner.size() <= 8;
    for (QChar character : banner) {
        valid = valid && character.unicode() >= 0x20 && character.unicode() <= 0x7e;
    }
    if (!valid) {
        emit maintenanceFailed(tr("The power-on banner must contain at most 8 printable ASCII characters."));
        return;
    }
    QByteArray data = banner.toLatin1();
    data.append('\0');
    beginMaintenance(MaintenanceOperation::BannerWrite, 0x50, int(data.size()), data);
}

void Kpa500Connection::restoreConfiguration(const QByteArray& data, const QString& expectedSerial,
                                          const QString& expectedFirmware)
{
    if (data.size() != 224 || expectedSerial.isEmpty() || expectedFirmware != "01.54"
        || m_status.serialNumber != expectedSerial || m_status.firmwareVersion != expectedFirmware) {
        emit maintenanceFailed(tr("The backup must contain 224 bytes and match this amplifier's serial number and firmware 01.54."));
        return;
    }
    beginMaintenance(MaintenanceOperation::ConfigurationRestore, 0, 224, data);
}

void Kpa500Connection::cancelMaintenance()
{
    if (maintenanceBusy()) {
        failMaintenance(tr("Amplifier maintenance cancelled."));
    }
}

void Kpa500Connection::sendMaintenanceRequest(const QByteArray& request)
{
    if (!maintenanceBusy()) {
        return;
    }
    if (request.isEmpty()) {
        failMaintenance(tr("Invalid amplifier maintenance request."));
        return;
    }
    m_maintenanceRequest = request;
    m_maintenanceReadRetries = 0;
    m_maintenanceTimer.start();
    sendRaw(request);
}

void Kpa500Connection::startMaintenancePreflight()
{
    // Serial and raw TCP preserve order. An identity round-trip fences ordinary
    // poll replies before the fresh ON/OS/WS exchange; no cached status authorizes EW.
    m_maintenancePhase = MaintenancePhase::PreflightIdentity;
    sendMaintenanceRequest(Kpa500::buildQuery("SN"));
}

void Kpa500Connection::sendMaintenanceBlock()
{
    if (!maintenanceBusy()) {
        return;
    }
    m_maintenanceExpectedLength = std::min(64, m_maintenanceLength - m_maintenanceOffset);
    const quint16 address = quint16(m_maintenanceStartAddress + m_maintenanceOffset);
    if (m_maintenancePhase == MaintenancePhase::Writing) {
        m_maintenanceWriteStarted = true;
        sendMaintenanceRequest(Kpa500::buildEepromWrite(address,
            m_maintenanceWriteData.mid(m_maintenanceOffset, m_maintenanceExpectedLength)));
    } else {
        sendMaintenanceRequest(Kpa500::buildEepromRead(address, m_maintenanceExpectedLength));
    }
}

void Kpa500Connection::handleMaintenanceStatus(const QString& command)
{
    if (!maintenanceBusy()) {
        return;
    }
    if (m_transmitting || m_status.powerOn == false
        || m_status.forwardPowerW.value_or(0.0f) > 0.0f
        || (maintenanceWriting() && m_status.operate == true)) {
        failMaintenance(tr("Amplifier maintenance stopped because power, Standby or transmit state changed."));
        return;
    }
    if ((command == "SN" && m_status.serialNumber != m_maintenanceSerial)
        || (command == "RVM" && m_status.firmwareVersion != m_maintenanceFirmware)) {
        failMaintenance(tr("Amplifier identity changed during maintenance."));
        return;
    }
    if (m_maintenancePhase == MaintenancePhase::PreflightIdentity && command == "SN") {
        m_maintenancePhase = MaintenancePhase::PreflightPower;
        sendMaintenanceRequest(Kpa500::buildQuery("ON"));
    } else if (m_maintenancePhase == MaintenancePhase::PreflightPower && command == "ON") {
        if (m_status.powerOn != true) {
            failMaintenance(tr("Fresh amplifier power status did not confirm ON."));
            return;
        }
        m_maintenancePhase = MaintenancePhase::PreflightStandby;
        sendMaintenanceRequest(Kpa500::buildQuery("OS"));
    } else if (m_maintenancePhase == MaintenancePhase::PreflightStandby && command == "OS") {
        if (m_status.operate != false) {
            failMaintenance(tr("Fresh amplifier status did not confirm Standby."));
            return;
        }
        m_maintenancePhase = MaintenancePhase::PreflightRf;
        sendMaintenanceRequest(Kpa500::buildQuery("WS"));
    } else if (m_maintenancePhase == MaintenancePhase::PreflightRf && command == "WS") {
        if (m_status.forwardPowerW != 0.0f) {
            failMaintenance(tr("Fresh amplifier status did not confirm zero RF power."));
            return;
        }
        for (const QString& field : {QStringLiteral("ON"), QStringLiteral("OS"), QStringLiteral("WS")}) {
            if (!m_received.contains(field) || m_clock.elapsed() - m_received.value(field) > 3000) {
                failMaintenance(tr("Fresh safety readback expired before the write; try again when the connection responds promptly."));
                return;
            }
        }
        m_maintenancePhase = MaintenancePhase::Writing;
        sendMaintenanceBlock();
    }
}

void Kpa500Connection::handleMaintenanceReply(const QString& command, const QString& argument)
{
    if (!maintenanceBusy()) {
        report("REJECT unsolicited EEPROM reply");
        return;
    }
    const std::optional<Kpa500::EepromReply> reply = Kpa500::parseEepromReply(command, argument);
    const bool expectingWrite = m_maintenancePhase == MaintenancePhase::Writing;
    const bool expectingRead = m_maintenancePhase == MaintenancePhase::Reading
        || m_maintenancePhase == MaintenancePhase::Verifying;
    if (!reply || (!expectingWrite && !expectingRead)
        || reply->writeAck != expectingWrite
        || reply->address != m_maintenanceStartAddress + m_maintenanceOffset
        || reply->length != m_maintenanceExpectedLength) {
        failMaintenance(tr("Invalid or unexpected EEPROM reply; maintenance stopped."));
        return;
    }
    m_maintenanceTimer.stop();
    if (!expectingWrite) {
        if (m_maintenancePhase == MaintenancePhase::Verifying
            && reply->data != m_maintenanceWriteData.mid(m_maintenanceOffset, m_maintenanceExpectedLength)) {
            failMaintenance(tr("Configuration readback did not match the requested bytes."));
            return;
        }
        m_maintenanceData.append(reply->data);
    }
    m_maintenanceOffset += m_maintenanceExpectedLength;
    const int done = m_maintenanceOffset
        + (m_maintenancePhase == MaintenancePhase::Verifying ? m_maintenanceLength : 0);
    emit maintenanceProgress(done, maintenanceWriting() ? 2 * m_maintenanceLength : m_maintenanceLength);
    if (!maintenanceBusy()) {
        return;
    }
    if (m_maintenanceOffset == m_maintenanceLength) {
        if (expectingWrite) {
            m_maintenancePhase = MaintenancePhase::Verifying;
            m_maintenanceOffset = 0;
            m_maintenanceData.clear();
            sendMaintenanceBlock();
        } else {
            finishMaintenance();
        }
    } else if (expectingWrite) {
        // Each block has a fresh ON/OS/WS preflight, even over a slow bridge.
        startMaintenancePreflight();
    } else {
        sendMaintenanceBlock();
    }
}

void Kpa500Connection::onMaintenanceTimeout()
{
    if (!maintenanceBusy()) {
        return;
    }
    if (m_maintenancePhase != MaintenancePhase::Writing && m_maintenanceReadRetries < 2) {
        ++m_maintenanceReadRetries;
        report("MAINTENANCE retrying read-only request");
        m_maintenanceTimer.start();
        sendRaw(m_maintenanceRequest);
        return;
    }
    failMaintenance(tr("Amplifier maintenance reply timed out."));
}

void Kpa500Connection::resetMaintenance()
{
    m_maintenanceTimer.stop();
    m_maintenanceOperation = MaintenanceOperation::None;
    m_maintenancePhase = MaintenancePhase::None;
    m_maintenanceRequest.clear();
    m_maintenanceData.clear();
    m_maintenanceWriteData.clear();
    m_maintenanceWriteStarted = false;
    m_parser.reset();
    if (m_connected) {
        // Preserve identity for the completed backup while reacquiring live state.
        Kpa500::Status identity;
        identity.serialNumber = m_status.serialNumber;
        identity.firmwareVersion = m_status.firmwareVersion;
        m_status = identity;
        m_received.clear();
        m_sentBand = m_failedBand = m_manualBand = -1;
        m_statusSeenSinceFastTick = false;
        m_silentTicks = 0;
        m_connectCmdIndex = 0;
        m_connectCmdsDone = false;
        enterPowerCheckMode();
        m_powerCheckPending = false;
        m_lastPowerCheckAt = m_clock.elapsed() - kPowerCheckMs;
        if (m_responding) {
            m_responding = false;
            emit respondingChanged(false);
        }
        emit statusUpdated(m_status);
        m_fastPollTimer.start();
        m_slowPollTimer.start();
    }
    emit maintenanceBusyChanged(false);
}

void Kpa500Connection::finishMaintenance()
{
    const MaintenanceOperation operation = m_maintenanceOperation;
    const QByteArray data = m_maintenanceData;
    const QByteArray bannerData = m_maintenanceWriteData;
    resetMaintenance();
    if (operation == MaintenanceOperation::ConfigurationRead) {
        emit configurationRead(data);
    } else if (operation == MaintenanceOperation::FaultHistoryRead) {
        emit faultHistoryRead(data);
    } else if (operation == MaintenanceOperation::BannerRead || operation == MaintenanceOperation::BannerWrite) {
        const QByteArray raw = operation == MaintenanceOperation::BannerWrite ? bannerData : data;
        const qsizetype terminator = raw.indexOf('\0');
        emit powerOnBannerRead(QString::fromLatin1(terminator < 0 ? raw : raw.left(terminator)));
    }
    const bool wrote = operation == MaintenanceOperation::BannerWrite
        || operation == MaintenanceOperation::ConfigurationRestore;
    const QString message = wrote
        ? tr("Configuration written and verified. Power-cycle the amplifier manually when ready.")
        : tr("Amplifier maintenance read completed.");
    report("MAINTENANCE " + message);
    emit maintenanceFinished(message);
}

void Kpa500Connection::failMaintenance(const QString& reason)
{
    if (!maintenanceBusy()) {
        return;
    }
    QString message = reason;
    if (m_maintenanceWriteStarted) {
        message += tr(" The operation is incomplete and amplifier memory may already have changed. No write will be resumed or retried automatically.");
    }
    resetMaintenance();
    report("MAINTENANCE " + message);
    emit maintenanceFailed(message);
}

void Kpa500Connection::enterPowerCheckMode()
{
    if (m_powerCheckOnly) { return; }
    m_powerCheckOnly = true;
    m_powerCheckPending = false;
    m_lastPowerCheckAt = m_clock.elapsed();
    m_sentBand = m_failedBand = m_manualBand = -1;
    report("POLL quiet mode; checking power every 5 seconds");
}

void Kpa500Connection::expireTelemetry()
{
    bool changed = false;
    for (auto it = m_received.begin(); it != m_received.end();) {
        const QString cmd = it.key();
        const qint64 limit = Kpa500::fastPollCommands().contains(cmd) ? 3000 : 8000;
        if (cmd == "RVM" || cmd == "SN" || m_clock.elapsed() - it.value() <= limit) { ++it; continue; }
        if (cmd == "WS") { m_status.forwardPowerW.reset(); m_status.swr.reset(); }
        if (cmd == "OS") { m_status.operate.reset(); }
        if (cmd == "BN") { m_status.band.reset(); }
        if (cmd == "ON") { m_status.powerOn.reset(); }
        if (cmd == "TM") { m_status.paTemperatureC.reset(); }
        if (cmd == "VI") { m_status.paVoltageV.reset(); m_status.paCurrentA.reset(); }
        if (cmd == "FL") { m_status.faultCode.reset(); }
        if (cmd == "FC") { m_status.fanMinSpeed.reset(); }
        if (cmd == "SP") { m_status.speakerOn.reset(); }
        report("STALE " + cmd + " reply expired");
        it = m_received.erase(it);
        changed = true;
    }
    if (changed) { emit statusUpdated(m_status); }
}
} // namespace AetherSDR
