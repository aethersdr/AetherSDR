#include "LpMeterConnection.h"
#include "LogManager.h"

#include <cmath>

namespace AetherSDR {

namespace {
// Link is up but the meter has gone quiet. Must exceed the longest silence the
// poll gate itself allows -- riding along stops at kMaxQuietMs, then our own
// poll needs a reply -- or the handover as a foreign poller stops trips it on
// every cycle (#6794).
constexpr int kDataTimeoutMs = static_cast<int>(LpMeter::PollGate::kMaxQuietMs) + 1000;
static_assert(kDataTimeoutMs > LpMeter::PollGate::kMaxQuietMs + LpMeter::PollGate::kSoloPollIntervalMs,
              "the data watchdog must outlast the poll gate's own longest silence");
// Reconnect cadence, matching AcomConnection/SpeConnection: retry forever
// until the meter returns or the operator disconnects.
constexpr int kReconnectMs = 5000;
// How long the poll gate must hold one state before it is worth reporting.
// Comfortably longer than the startup flap it exists to absorb.
constexpr int kGateSettleMs = 1500;
// Time the radio has to confirm one cable setting by status.
constexpr int kCableConfigureTimeoutMs = 5000;

// The watchdog grows with a slower solo interval so it still outlasts the
// poll gate's longest silence plus one of our own polls.
int dataTimeoutMs(qint64 soloIntervalMs)
{
    return kDataTimeoutMs
        + static_cast<int>(soloIntervalMs - LpMeter::PollGate::kSoloPollIntervalMs);
}

QString cableChangeText(const LpMeter::CableChange& change)
{
    using Setting = LpMeter::CableChange::Setting;
    switch (change.setting) {
        case Setting::Type:        return QStringLiteral("type %1").arg(change.text);
        case Setting::Speed:       return QStringLiteral("speed %1").arg(change.number);
        case Setting::DataBits:    return QStringLiteral("data bits %1").arg(change.number);
        case Setting::Parity:      return QStringLiteral("parity %1").arg(change.text);
        case Setting::StopBits:    return QStringLiteral("stop bits %1").arg(change.number);
        case Setting::FlowControl: return QStringLiteral("flow control %1").arg(change.text);
    }
    return QString();
}
}  // namespace

qint64 LpMeterConnection::nowMs() const
{
    return m_clock.elapsed();
}

LpMeterConnection::LpMeterConnection(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    connect(&m_socket, &QTcpSocket::connected, this, &LpMeterConnection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &LpMeterConnection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &LpMeterConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    m_parser.setReadingCallback([this](const LpMeter::Reading& r) { onReading(r); });

    m_range.setCeilings(m_ceilings,
                        LpMeter::RangeTracker::CeilingSource::ConfigLoad);
    m_range.reset();

    // The poll tick runs at the solo rate; PollGate decides per tick whether
    // a poll is actually sent. See its header for why gating on FOREIGN
    // records rather than on all records is what keeps the solo rate and the
    // suppression threshold independent.
    m_pollTimer.setInterval(static_cast<int>(LpMeter::PollGate::kSoloPollIntervalMs));
    connect(&m_pollTimer, &QTimer::timeout, this, &LpMeterConnection::pollTick);

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected) { return; }
        // Re-read the flag rather than trusting that arming implied it. Both
        // halves are needed: setAutoReconnect() stops an armed timer, and
        // this guard covers any other route to a fired-but-stale timer.
        if (!m_autoReconnect) { return; }
        if (m_mode == Mode::Network && !m_lastHost.isEmpty()) {
            connectNetwork(m_lastHost, m_lastPort);
#ifdef HAVE_SERIALPORT
        } else if (m_mode == Mode::Serial && !m_lastSerialPort.isEmpty()) {
            connectSerial(m_lastSerialPort);
#endif
        }
    });

    m_gateSettleTimer.setSingleShot(true);
    m_gateSettleTimer.setInterval(kGateSettleMs);
    connect(&m_gateSettleTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        // Only report a state that actually differs from the last one logged.
        // The gate flips briefly now and then even in steady state, because
        // over TCP we are timing record ARRIVAL rather than the foreign
        // client's true cadence: segment batching delivers two records
        // together and then nothing for twice the interval, which reads as a
        // gap no fixed multiple of the mean can absorb. One extra poll every
        // few seconds is harmless; logging it as a state change is not.
        if (m_gateStateReported && m_ridingAlongSeen == m_ridingAlongReported) {
            return;
        }
        m_ridingAlongReported = m_ridingAlongSeen;
        m_gateStateReported = true;
        if (m_ridingAlongSeen) {
            const qint64 iv = m_gate.foreignIntervalMs();
            if (iv > 0) {
                qCInfo(lcTuner) << "LpMeterConnection: another client is polling this"
                                   " meter every" << iv << "ms — riding along on its"
                                   " replies and sending no polls of our own.";
            } else {
                qCInfo(lcTuner) << "LpMeterConnection: another client is polling this"
                                   " meter (cadence not yet established) — riding"
                                   " along on its replies.";
            }
        } else {
            qCInfo(lcTuner) << "LpMeterConnection: no other client polling this meter"
                               " — polling it ourselves every"
                            << m_gate.soloIntervalMs() << "ms.";
        }
    });

    m_cableConfigureTimer.setSingleShot(true);
    m_cableConfigureTimer.setInterval(kCableConfigureTimeoutMs);
    connect(&m_cableConfigureTimer, &QTimer::timeout, this, [this]() {
        if (!m_radioConfiguring || !m_lastCableChange) { return; }
        failRadioCable(QStringLiteral("the radio did not apply %1 to USB cable %2")
                           .arg(cableChangeText(*m_lastCableChange), m_lastRadioSerial));
    });

    m_dataWatchdog.setSingleShot(true);
    m_dataWatchdog.setInterval(kDataTimeoutMs);
    connect(&m_dataWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        // Deliberately does NOT drop the link. The meter wedging with the
        // transport healthy is a real observed state, and tearing down the
        // socket would take the applet tile away at the exact moment the
        // operator needs to be told the meter stopped answering.
        //
        // Says how long ago our last poll went out. The watchdog outlasts the
        // poll gate's longest ride-along, so by now we should be polling: a
        // recent accepted poll helps distinguish a silent meter from a poll
        // loop that stopped writing (e.g. the main thread stalled).
        // A support bundle cannot otherwise tell those apart.
        const qint64 now = nowMs();
        if (m_silenceSinceMs < 0) {
            m_silenceSinceMs = (m_lastRecordMs >= 0) ? m_lastRecordMs : m_transportUpMs;
        }
        qCWarning(lcTuner).noquote()
            << QStringLiteral("LpMeterConnection: link up but no valid record in %1 s"
                              " (%2) — meter may be wedged (it can stop answering"
                              " with the serial link perfectly healthy) or the records"
                              " may be unparseable. Power-cycling the meter clears"
                              " the former.")
                   .arg(m_dataWatchdog.interval() / 1000.0)
                   .arg(m_lastPollMs >= 0
                            ? QStringLiteral("our last poll %1 ms ago").arg(now - m_lastPollMs)
                            : QStringLiteral("no poll of ours sent since connect"));
        setDataFlowing(false);
    });
}

QString LpMeterConnection::description() const
{
    if (m_mode == Mode::Network) {
        return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
    }
#ifdef HAVE_SERIALPORT
    if (m_mode == Mode::Serial) {
        return m_lastSerialPort;
    }
#endif
    if (m_mode == Mode::Radio) {
        return QStringLiteral("radio USB cable %1").arg(m_lastRadioSerial);
    }
    return QString();
}

QString LpMeterConnection::sourceLabel() const
{
    switch (m_mode) {
        case Mode::Network: return QStringLiteral("NETWORK");
        case Mode::Serial:  return QStringLiteral("SERIAL");
        case Mode::Radio:   return QStringLiteral("RADIO");
        default:            return QStringLiteral("—");
    }
}

void LpMeterConnection::setAutoReconnect(bool on)
{
    m_autoReconnect = on;
    if (!on) {
        // Cancel a retry already counting down; the flag alone does not
        // reach it.
        m_reconnectTimer.stop();
        if (m_mode == Mode::Radio && m_radioResumeAutomatic && !m_connected) {
            m_radioWanted = false;
            m_radioConfiguring = false;
            m_radioMayRetype = false;
            m_radioResumeAutomatic = false;
            m_cableConfigureTimer.stop();
            m_lastCableChange.reset();
        }
    }
}

void LpMeterConnection::setRangeCeilings(const LpMeter::RangeCeilings& ceilings,
                                        LpMeter::RangeTracker::CeilingSource source,
                                        std::optional<int> editedRange)
{
    m_ceilings = ceilings;
    const double before = m_range.ceilingW();
    const bool beforeAutoExpanded = m_range.ceilingAutoExpanded();
    // RangeTracker applies its up-only guard to a ConfigLoad only. An
    // OperatorEdit wins for the displayed range (or a reset of all ranges),
    // including when it lowers an auto-expanded ceiling.
    m_range.setCeilings(ceilings, source, editedRange);
    if (std::abs(before - m_range.ceilingW()) > 1e-6
        || beforeAutoExpanded != m_range.ceilingAutoExpanded()) {
        emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
    }
}

void LpMeterConnection::setPollIntervalMs(qint64 ms)
{
    m_gate.setSoloIntervalMs(ms);
    m_pollTimer.setInterval(static_cast<int>(m_gate.soloIntervalMs()));
    m_dataWatchdog.setInterval(dataTimeoutMs(m_gate.soloIntervalMs()));
}

#ifdef HAVE_SERIALPORT
void LpMeterConnection::connectSerial(const QString& portName)
{
    // Retire the old session before changing its mode or reconnect target.
    // QSerialPort::close() does not emit disconnected(), so teardownDevice()
    // alone leaves m_connected and the reconnect lifecycle stale.
    prepareConnection(Mode::Serial, portName);
    m_deliberateDisconnect = false;

    if (!m_serialPort) {
        m_serialPort = new QSerialPort(this);
        connect(m_serialPort, &QSerialPort::readyRead, this, &LpMeterConnection::onReadyRead);
        connect(m_serialPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError err) {
            if (err == QSerialPort::NoError) { return; }
            const QString msg = m_serialPort->errorString();
            qCWarning(lcTuner) << "LpMeterConnection: serial error" << err << msg;
            onTransportError(msg);
            if (m_connected) { onTransportDown(); }
        });
    }

    m_serialPort->setPortName(portName);
    // Fixed by the meter's own protocol spec — not user-configurable.
    m_serialPort->setBaudRate(QSerialPort::Baud115200);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        const QString err = m_serialPort->errorString();
        qCWarning(lcTuner) << "LpMeterConnection: failed to open" << portName << err;
        emit connectionFailed(err);
        if (m_autoReconnect) { armReconnect(); }
        return;
    }
    // DELIBERATELY does not touch DTR/RTS — unlike AcomConnection, which
    // forces both low to stay clear of its amplifier's power-button logic.
    // The reference station's working path runs through ser2net with
    // `local -rtscts` and CLOCAL, i.e. modem control lines ignored entirely,
    // so the proven configuration is to leave them alone.

    m_device = m_serialPort;
    onTransportUp();
}
#endif

void LpMeterConnection::connectNetwork(const QString& host, quint16 port)
{
    // Also aborts an in-flight previous TCP attempt under the deliberate-
    // disconnect guard, so its teardown cannot arm a retry for the old host.
    prepareConnection(Mode::Network, host, port);
    m_deliberateDisconnect = false;

    m_device = &m_socket;
    qCDebug(lcTuner) << "LpMeterConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
}

void LpMeterConnection::setRadioCableHooks(RadioCableHooks hooks)
{
    m_radioHooks = std::move(hooks);
}

void LpMeterConnection::connectRadioCable(const QString& serial, RadioCableIntent intent)
{
    prepareConnection(Mode::Radio, serial);
    m_deliberateDisconnect = false;
    m_radioWanted = true;
    m_radioConfiguring = true;
    m_radioResumeAutomatic = false;
    m_lastCableChange.reset();
    const LpMeter::CableSnapshot cable = m_radioHooks.snapshot
        ? m_radioHooks.snapshot(serial) : LpMeter::CableSnapshot{};
    // A cable that appears later starts its own cycle, which may not retype.
    m_radioCablePresent = cable.known && cable.pluggedIn;
    m_radioMayRetype = intent == RadioCableIntent::Operator && m_radioCablePresent;
    if (!cable.known || !cable.pluggedIn) {
        // Not final: the link still comes up when the radio reports the cable.
        const QString reason = cable.known
            ? QStringLiteral("USB cable %1 is not plugged in to the radio; waiting for it").arg(serial)
            : QStringLiteral("the radio has not reported USB cable %1; waiting for it").arg(serial);
        qCInfo(lcTuner).noquote() << "LpMeterConnection:" << reason;
        emit connectionFailed(reason);
    }
    evaluateRadioCable(cable);
}

void LpMeterConnection::onRadioCableStatus(const QString& serial,
                                           const LpMeter::CableSnapshot& cable)
{
    if (m_mode != Mode::Radio || !m_radioWanted || serial != m_lastRadioSerial) { return; }
    evaluateRadioCable(cable);
}

void LpMeterConnection::onRadioCableData(const QString& serial, const QByteArray& bytes)
{
    if (m_mode != Mode::Radio || !m_connected || serial != m_lastRadioSerial) { return; }
    ingest(bytes);
}

void LpMeterConnection::evaluateRadioCable(const LpMeter::CableSnapshot& cable)
{
    const bool present = cable.known && cable.pluggedIn;
    if (present && !m_radioCablePresent) {
        // Plugged in, or the radio (re)connected: configure it again.
        m_radioConfiguring = true;
        m_radioMayRetype = false;
        m_lastCableChange.reset();
    }
    m_radioCablePresent = present;

    if (LpMeter::cableReady(cable)) {
        m_cableConfigureTimer.stop();
        m_radioConfiguring = false;
        m_radioMayRetype = false;
        m_radioResumeAutomatic = false;
        m_lastCableChange.reset();
        if (!m_connected) { onTransportUp(); }
        return;
    }
    if (m_connected) {
        // Unplugged, the radio went away, or another client changed the cable.
        // The last is not fought: it waits for a replug or an operator connect.
        qCInfo(lcTuner) << "LpMeterConnection: USB cable" << m_lastRadioSerial
                        << (present ? "is no longer passthrough 115200 8N1" : "is gone");
        m_radioConfiguring = false;
        // Like the serial and network modes: without auto-reconnect, a
        // dropped link stays down until the operator connects again.
        if (!m_autoReconnect) { m_radioWanted = false; }
        else { m_radioResumeAutomatic = true; }
        onTransportDown();
    }
    if (!present || !m_radioConfiguring) { return; }
    if (!m_radioMayRetype && cable.type != QLatin1String("cat")
        && cable.type != QLatin1String("passthrough")) {
        // Left alone: only an operator Connect may retype it.
        m_radioWanted = false;
        m_radioResumeAutomatic = false;
        failRadioCable(QStringLiteral("USB cable %1 is now a %2 cable; press Connect to set it to passthrough")
                           .arg(m_lastRadioSerial,
                                cable.type.isEmpty() ? QStringLiteral("non-passthrough") : cable.type));
        return;
    }

    const std::optional<LpMeter::CableChange> change = LpMeter::nextCableChange(cable);
    if (!change) { return; }
    if (m_lastCableChange == change) {
        return;  // sent; waiting for the radio's status to confirm it
    }
    m_lastCableChange = change;
    qCInfo(lcTuner) << "LpMeterConnection: setting USB cable" << m_lastRadioSerial
                    << "to" << cableChangeText(*change);
    if (!m_radioHooks.configure || !m_radioHooks.configure(m_lastRadioSerial, *change)) {
        failRadioCable(QStringLiteral("the radio refused %1 for USB cable %2")
                           .arg(cableChangeText(*change), m_lastRadioSerial));
        return;
    }
    m_cableConfigureTimer.start();
}

void LpMeterConnection::failRadioCable(const QString& reason)
{
    m_radioConfiguring = false;
    m_radioMayRetype = false;
    m_cableConfigureTimer.stop();
    m_lastCableChange.reset();
    qCWarning(lcTuner).noquote() << "LpMeterConnection:" << reason;
    emit connectionFailed(reason);
}

void LpMeterConnection::prepareConnection(Mode mode, const QString& endpoint, quint16 port)
{
    const bool sameTarget = m_mode == mode
        && (mode == Mode::Network
                ? m_lastHost == endpoint && m_lastPort == port
                : mode == Mode::Radio ? m_lastRadioSerial == endpoint
                                      : m_lastSerialPort == endpoint);
    disconnect();
    if (!sameTarget) {
        m_silenceSinceMs = -1;
    }
    m_mode = mode;
    if (mode == Mode::Network) {
        m_lastHost = endpoint;
        m_lastPort = port;
    } else if (mode == Mode::Radio) {
        m_lastRadioSerial = endpoint;
    } else {
        m_lastSerialPort = endpoint;
    }
}

void LpMeterConnection::disconnect()
{
    // Self-contained: don't depend on teardownDevice() indirectly triggering
    // onTransportDown(). It sometimes does (QTcpSocket::abort() synchronously
    // emits disconnected()) and sometimes doesn't (QSerialPort::close() emits
    // nothing at all) — relying on that is what left AcomConnection's applet
    // stuck showing "Connected" after a user-initiated disconnect.
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_dataWatchdog.stop();
    m_gateSettleTimer.stop();
    m_cableConfigureTimer.stop();
    m_radioWanted = false;
    m_radioConfiguring = false;
    m_radioCablePresent = false;
    m_radioMayRetype = false;
    m_radioResumeAutomatic = false;
    m_lastCableChange.reset();
    m_connected = false;
    teardownDevice();
    m_parser.reset();
    m_gate.reset();
    if (wasConnected || m_dataFlowStateKnown) {
        setDataFlowing(false);
    } else {
        m_dataFlowing = false;
    }
    if (wasConnected) {
        qCDebug(lcTuner) << "LpMeterConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void LpMeterConnection::teardownDevice()
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

void LpMeterConnection::onTransportUp()
{
    m_connected = true;
    m_bytesWithoutRecords = false;
    m_ridingAlongSeen = false;
    m_ridingAlongReported = false;
    m_gateStateReported = false;
    m_lastRecordMs = -1;
    m_transportUpMs = nowMs();
    m_lastPollMs = -1;
    m_firstBytesMs = -1;
    m_dataFlowing = false;
    m_dataFlowStateKnown = false;
    m_gate.reset();
    m_range.setCeilings(m_ceilings,
                        LpMeter::RangeTracker::CeilingSource::ConfigLoad);
    m_range.reset();

    qCInfo(lcTuner) << "LpMeterConnection: connected via" << description();
    m_pollTimer.start();
    m_dataWatchdog.start();

    emit connected();
    emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
}

void LpMeterConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_pollTimer.stop();
    m_dataWatchdog.stop();
    m_gateSettleTimer.stop();
    m_parser.reset();
    m_gate.reset();
    setDataFlowing(false);
    if (wasConnected) {
        qCDebug(lcTuner) << "LpMeterConnection: disconnected";
        emit disconnected();
    }
    // A radio cable follows the radio's status instead of a retry timer.
    if (!m_deliberateDisconnect && m_autoReconnect && m_mode != Mode::Radio) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void LpMeterConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "LpMeterConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected && m_mode != Mode::Radio) {
        armReconnect();
    }
}

void LpMeterConnection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void LpMeterConnection::onReadyRead()
{
    if (!m_device) { return; }
    ingest(m_device->readAll());
}

void LpMeterConnection::ingest(const QByteArray& chunk)
{
    if (chunk.isEmpty()) { return; }
    m_parser.feed(chunk);

    // Bytes arriving but nothing decoding. With no checksum on this protocol
    // there is no other signal that the link is alive but the content is
    // wrong (a firmware predating 1.2.0.0, a telnet-mode proxy mangling the
    // stream, or a genuinely corrupt link). Latched so it logs on the
    // transition, not per chunk.
    if (m_firstBytesMs < 0) { m_firstBytesMs = nowMs(); }
    if (m_lastRecordMs < 0 && !m_bytesWithoutRecords
        && nowMs() - m_firstBytesMs > m_dataWatchdog.interval()) {
        m_bytesWithoutRecords = true;
        qCWarning(lcTuner) << "LpMeterConnection: receiving bytes but decoding no"
                              " records. Check the proxy is in RAW mode (not telnet)"
                              " and that the meter's firmware is 1.2.0.0 or later"
                              " (earlier versions use a different baud rate and"
                              " omit dBm/SWR).";
    }
}

void LpMeterConnection::pollTick()
{
    if (!m_connected || (m_mode != Mode::Radio && !m_device)) { return; }

    const qint64 now = nowMs();
    const bool poll = m_gate.shouldPoll(now);

    // Report the gate's state only once it settles (m_gateSettleTimer). Restart only
    // on an actual change, plus once for the FIRST state after connect (which equals
    // m_ridingAlongSeen's initial value) so the opening POLLING/SHARED line reaches
    // the support bundle; m_gateStateReported latches that.
    if (m_gate.isRidingAlong() != m_ridingAlongSeen
        || (!m_gateStateReported && !m_gateSettleTimer.isActive())) {
        m_ridingAlongSeen = m_gate.isRidingAlong();
        m_gateSettleTimer.start();
    }

    if (!poll) { return; }
    if (writePoll()) {
        m_lastPollMs = now;
    }
}

bool LpMeterConnection::writePoll()
{
    const QByteArray poll(1, LpMeter::kPollCommand);
    if (m_mode == Mode::Radio) {
        return m_radioHooks.write && m_radioHooks.write(m_lastRadioSerial, poll);
    }
    return m_device && m_device->write(poll) == 1;
}

void LpMeterConnection::onReading(const LpMeter::Reading& reading)
{
    const qint64 now = nowMs();
    m_gate.onRecord(now);
    m_lastRecordMs = now;
    m_bytesWithoutRecords = false;

    const double before = m_range.ceilingW();
    m_range.onReading(reading.powerRange, reading.powerW, now);
    if (std::abs(before - m_range.ceilingW()) > 1e-6) {
        emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
    }

    m_lastReading = reading;
    setDataFlowing(true);
    m_dataWatchdog.start();  // restart: a valid record is the liveness proof
    emit readingUpdated(reading);
}

void LpMeterConnection::setDataFlowing(bool flowing)
{
    if (m_dataFlowStateKnown && m_dataFlowing == flowing) { return; }
    m_dataFlowing = flowing;
    m_dataFlowStateKnown = true;
    if (flowing) {
        // A recovery that closes a logged dim is logged at the dim's level, so
        // a default (warning-level) support bundle shows both sides of every
        // outage rather than a run of dims that never end.
        if (m_silenceSinceMs >= 0) {
            qCWarning(lcTuner).noquote()
                << QStringLiteral("LpMeterConnection: records flowing again after %1 s"
                                  " without one.")
                       .arg((nowMs() - m_silenceSinceMs) / 1000.0, 0, 'f', 1);
            m_silenceSinceMs = -1;
        } else {
            qCInfo(lcTuner) << "LpMeterConnection: records flowing again.";
        }
    }
    emit dataFlowingChanged(flowing);
}

}  // namespace AetherSDR
