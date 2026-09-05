#include "LpMeterConnection.h"
#include "LogManager.h"

#include <QDateTime>

#include <cmath>

namespace AetherSDR {

namespace {
// Link is up but the meter has gone quiet. Generous relative to the 100 ms
// poll interval so a brief hiccup, or the handover as a foreign poller stops
// and we take over, never trips it.
constexpr int kDataTimeoutMs = 2000;
// Reconnect cadence, matching AcomConnection/SpeConnection: retry forever
// until the meter returns or the operator disconnects.
constexpr int kReconnectMs = 5000;
// How long the poll gate must hold one state before it is worth reporting.
// Comfortably longer than the startup flap it exists to absorb.
constexpr int kGateSettleMs = 1500;
}  // namespace

qint64 LpMeterConnection::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

LpMeterConnection::LpMeterConnection(QObject* parent)
    : QObject(parent)
{
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
                            << LpMeter::PollGate::kSoloPollIntervalMs << "ms.";
        }
    });

    m_dataWatchdog.setSingleShot(true);
    m_dataWatchdog.setInterval(kDataTimeoutMs);
    connect(&m_dataWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        // Deliberately does NOT drop the link. The meter wedging with the
        // transport healthy is a real observed state, and tearing down the
        // socket would take the applet tile away at the exact moment the
        // operator needs to be told the meter stopped answering.
        qCWarning(lcTuner) << "LpMeterConnection: link up but no valid record in"
                           << kDataTimeoutMs / 1000.0
                           << "s — meter may be wedged (it can stop answering with"
                              " the serial link perfectly healthy) or the records"
                              " may be unparseable. Power-cycling the meter clears"
                              " the former.";
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
    return QString();
}

QString LpMeterConnection::sourceLabel() const
{
    switch (m_mode) {
        case Mode::Network: return QStringLiteral("NETWORK");
        case Mode::Serial:  return QStringLiteral("SERIAL");
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

#ifdef HAVE_SERIALPORT
void LpMeterConnection::connectSerial(const QString& portName)
{
    // Retire the old session before changing its mode or reconnect target.
    // QSerialPort::close() does not emit disconnected(), so teardownDevice()
    // alone leaves m_connected and the reconnect lifecycle stale.
    disconnect();
    m_mode = Mode::Serial;
    m_lastSerialPort = portName;
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
    disconnect();
    m_mode = Mode::Network;
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;

    m_device = &m_socket;
    qCDebug(lcTuner) << "LpMeterConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
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
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void LpMeterConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "LpMeterConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
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
    const QByteArray chunk = m_device->readAll();
    if (chunk.isEmpty()) { return; }
    m_parser.feed(chunk);

    // Bytes arriving but nothing decoding. With no checksum on this protocol
    // there is no other signal that the link is alive but the content is
    // wrong (a firmware predating 1.2.0.0, a telnet-mode proxy mangling the
    // stream, or a genuinely corrupt link). Latched so it logs on the
    // transition, not per chunk.
    if (m_firstBytesMs < 0) { m_firstBytesMs = nowMs(); }
    if (m_lastRecordMs < 0 && !m_bytesWithoutRecords
        && nowMs() - m_firstBytesMs > kDataTimeoutMs) {
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
    if (!m_connected || !m_device) { return; }

    const qint64 now = nowMs();
    const bool poll = m_gate.shouldPoll(now);

    // Report the gate's state only once it settles -- see m_gateSettleTimer.
    //
    // Restart ONLY on an actual change. The condition used to include
    // `|| !m_gateSettleTimer.isActive()`, which re-armed the timer on the
    // first tick after every expiry: it fired, the next tick restarted it, it
    // fired 1.5 s later, forever. The timeout early-returns when the state has
    // not changed, so those wakeups did nothing but cost a timer event every
    // ~1.6 s for the life of the connection. A state change is the only thing
    // that ever needs debouncing, and it re-arms the timer by itself.
    //
    // The second clause keeps the one thing the old condition got right: the
    // FIRST state after a connect must still be reported, and at that point
    // the gate agrees with m_ridingAlongSeen's initial value, so a
    // change-only test would never arm the timer and the opening
    // POLLING/SHARED line would be lost from the support bundle.
    // m_gateStateReported latches on the first report, so this arms once per
    // connection and never again.
    if (m_gate.isRidingAlong() != m_ridingAlongSeen
        || (!m_gateStateReported && !m_gateSettleTimer.isActive())) {
        m_ridingAlongSeen = m_gate.isRidingAlong();
        m_gateSettleTimer.start();
    }

    if (!poll) { return; }
    m_device->write(QByteArray(1, LpMeter::kPollCommand));
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
        qCInfo(lcTuner) << "LpMeterConnection: records flowing again.";
    }
    emit dataFlowingChanged(flowing);
}

}  // namespace AetherSDR
