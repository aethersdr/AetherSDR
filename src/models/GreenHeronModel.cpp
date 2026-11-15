#include "models/GreenHeronModel.h"

#include "core/LogManager.h"

#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR {

using namespace GreenHeron;

GreenHeronModel::GreenHeronModel(QObject* parent)
    : QObject(parent)
{
    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setInterval(kKeepAliveIntervalMs);
    connect(m_keepAliveTimer, &QTimer::timeout, this, &GreenHeronModel::onKeepAlive);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &GreenHeronModel::onReconnect);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    m_connectTimer->setInterval(kConnectTimeoutMs);
    connect(m_connectTimer, &QTimer::timeout, this, &GreenHeronModel::onConnectTimeout);

    m_rotorSilenceTimer = new QTimer(this);
    m_rotorSilenceTimer->setInterval(kRotorPollMs);
    connect(m_rotorSilenceTimer, &QTimer::timeout,
            this, &GreenHeronModel::onRotorSilenceTick);

    m_clock.start();
}

GreenHeronModel::~GreenHeronModel()
{
    m_wantConnected = false;
    teardownSocket();
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void GreenHeronModel::connectToHost(const QString& host, quint16 port)
{
    if (host.trimmed().isEmpty()) {
        setLastError(tr("No host address configured"));
        return;
    }

    m_host = host.trimmed();
    m_port = port == 0 ? kDefaultPort : port;
    m_wantConnected = true;
    m_backoffMs = kBackoffStartMs;
    m_reconnectTimer->stop();

    openSocket();
}

void GreenHeronModel::disconnectFromHost()
{
    m_wantConnected = false;
    m_reconnectTimer->stop();
    teardownSocket();

    // A deliberate disconnect is not a blip: drop the roster rather than
    // leaving a stale panel the operator might still act on.
    m_switches.clear();
    m_announced.clear();
    m_rotors.clear();
    m_rotorOrder.clear();
    m_liveRotors.clear();
    m_announcedDevices.clear();
    m_rotorSilenceTimer->stop();
    m_pending.clear();
    m_connected = false;
    m_stale = false;
    m_awaitingReplay = false;
    m_lastError.clear();

    emit connectionStateChanged();
    emit panelChanged();
}

void GreenHeronModel::openSocket()
{
    teardownSocket();

    // Armed here rather than in onSocketConnected() so it also covers the
    // attempt that never reaches ESTABLISHED: nothing may be sent, and no
    // retained panel may be acted on, until the device speaks on THIS socket.
    m_awaitingReplay = true;

    // Rotator state belongs to a socket, and only to a socket. The switch
    // roster is retained here and flagged stale on purpose; a heading is not,
    // because the operator would be looking at a number from the connection
    // before last with nothing on the wire to contradict it. See the header.
    m_rotors.clear();
    m_rotorOrder.clear();
    m_liveRotors.clear();
    m_announcedDevices.clear();

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &GreenHeronModel::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &GreenHeronModel::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &GreenHeronModel::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &GreenHeronModel::onSocketError);

    m_pending.clear();
    qCDebug(lcDevices) << "GreenHeron: connecting to" << m_host << ":" << m_port;
    m_connectTimer->start();
    m_socket->connectToHost(m_host, m_port);
}

void GreenHeronModel::teardownSocket()
{
    m_keepAliveTimer->stop();
    m_connectTimer->stop();
    m_rotorSilenceTimer->stop();
    if (m_socket == nullptr) {
        return;
    }

    QTcpSocket* socket = m_socket;
    m_socket = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void GreenHeronModel::scheduleReconnect()
{
    if (!m_wantConnected || m_reconnectTimer->isActive()) {
        return;
    }
    qCDebug(lcDevices) << "GreenHeron: reconnecting in" << m_backoffMs << "ms";
    m_reconnectTimer->start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, kBackoffMaxMs);
}

void GreenHeronModel::onReconnect()
{
    if (!m_wantConnected) {
        return;
    }
    openSocket();
}

// ── Socket events ───────────────────────────────────────────────────────────

void GreenHeronModel::onConnectTimeout()
{
    if (m_socket == nullptr || m_connected) {
        return;
    }
    qCWarning(lcDevices) << "GreenHeron: connect to" << m_host << "timed out";
    setLastError(tr("Connection timed out"));
    teardownSocket();
    scheduleReconnect();
}

void GreenHeronModel::onSocketConnected()
{
    m_connectTimer->stop();
    m_connected = true;
    // NOT cleared here. A reconnect that retained a roster is still showing a
    // pre-drop guess until the device replays it; clearing on the TCP event
    // opened a window where that guess was live and clickable. m_stale drops
    // in applyRecord(), when the device has actually vouched for the panel.
    m_stale = !m_switches.isEmpty();
    m_backoffMs = kBackoffStartMs;
    m_lastError.clear();
    m_keepAliveTimer->start();

    qCDebug(lcDevices) << "GreenHeron: connected to" << m_host << ":" << m_port;
    emit connectionStateChanged();
}

void GreenHeronModel::onSocketDisconnected()
{
    m_keepAliveTimer->stop();
    if (!m_connected && !m_wantConnected) {
        return;
    }

    m_connected = false;
    // Keep the last known panel visible but flagged, rather than blanking on
    // every blip. The view greys it out; nothing can be selected while stale.
    m_stale = !m_switches.isEmpty();

    qCDebug(lcDevices) << "GreenHeron: disconnected from" << m_host;
    emit connectionStateChanged();
    scheduleReconnect();
}

void GreenHeronModel::onSocketError()
{
    if (m_socket == nullptr) {
        return;
    }
    setLastError(m_socket->errorString());
    qCWarning(lcDevices) << "GreenHeron:" << m_host << "-" << m_socket->errorString();

    m_connected = false;
    m_stale = !m_switches.isEmpty();
    m_keepAliveTimer->stop();
    m_connectTimer->stop();
    emit connectionStateChanged();

    // disconnected() does not always follow an error — a refused connect never
    // reached ESTABLISHED, and an established socket can fail (host
    // unreachable, a reset seen mid-read) without Qt emitting it either. Retry
    // unconditionally rather than guessing which errors are followed by a
    // disconnect: scheduleReconnect() is idempotent through the timer's
    // isActive() guard, and openSocket() tears the old socket down first, so
    // the doubled call from the disconnect path costs nothing. Guarding on
    // wasConnected here left a live session that errored without a disconnect
    // with no retry ever scheduled.
    scheduleReconnect();
}

void GreenHeronModel::onSocketReadyRead()
{
    if (m_socket == nullptr) {
        return;
    }

    m_pending += m_socket->readAll();

    bool dropped = false;
    const QVector<QByteArray> records = splitRecords(m_pending, &dropped);
    if (dropped) {
        qCWarning(lcDevices)
            << "GreenHeron: discarded oversized/unterminated data from" << m_host
            << "- peer may not be speaking this protocol";
    }
    if (records.isEmpty()) {
        return;
    }

    const bool wasAwaitingReplay = m_awaitingReplay;
    for (const QByteArray& raw : records) {
        applyRecord(parse(raw));
    }
    emit panelChanged();
    // The gate opening changes what the panel is allowed to do, not just what
    // it shows. Anything wired only to connectionStateChanged — the natural
    // place to watch enablement from — would otherwise stay disabled forever.
    if (wasAwaitingReplay && !m_awaitingReplay) {
        emit connectionStateChanged();
    }
}

void GreenHeronModel::onKeepAlive()
{
    if (m_socket == nullptr || !m_connected) {
        return;
    }
    const char byte = kKeepAliveByte;
    m_socket->write(&byte, 1);
}

// ── State ───────────────────────────────────────────────────────────────────

void GreenHeronModel::noteAuthoritativeState()
{
    m_awaitingReplay = false;
    m_stale = false;
}

void GreenHeronModel::applyRecord(const Record& record)
{
    // The empty-name guard sits inside the three known verbs, not above the
    // switch: an unknown verb never has a switch name parsed out of it, so
    // guarding first made the Unknown branch below unreachable and dropped the
    // record silently.
    switch (record.type) {
    case RecordType::SwitchAdd: {
        if (record.switchName.isEmpty()) {
            return;
        }
        noteAuthoritativeState();
        GreenHeronSwitchState& state = m_switches[record.switchName];
        state.name = record.switchName;
        QStringList ports;
        ports.reserve(record.ports.size());
        for (const Port& port : record.ports) {
            ports.append(port.name);
        }
        state.ports = ports;
        // First-seen SWITCHADD order fixes the lock-slot mapping, so this list
        // is append-only: a reconnect replaying the roster must not reorder it.
        if (!m_announced.contains(record.switchName)) {
            m_announced.append(record.switchName);
        }
        break;
    }
    case RecordType::SwitchUpdate: {
        if (record.switchName.isEmpty()) {
            return;
        }
        noteAuthoritativeState();
        GreenHeronSwitchState& state = m_switches[record.switchName];
        state.name = record.switchName;
        state.selected = record.selected;
        state.wirelessSignal = record.wirelessSignal;
        break;
    }
    case RecordType::SwitchLocks: {
        if (record.switchName.isEmpty()) {
            return;
        }
        noteAuthoritativeState();
        GreenHeronSwitchState& state = m_switches[record.switchName];
        state.name = record.switchName;
        state.locks = record.locks;
        break;
    }
    case RecordType::DeviceAdd: {
        // ADD is a generic device announcement, not a rotator one — the field
        // is whatever the operator named the device. It is corroboration for a
        // rotator POINT has already established, never the thing that creates
        // one: a device that announces itself and never reports a heading is
        // not something this tile can drive.
        if (record.deviceName.isEmpty()) {
            return;
        }
        // Recorded in a list of its own rather than written onto the rotor,
        // because on real hardware THIS ARRIVES FIRST: the announcement lands
        // within ~100 ms of connect and the first POINT about a second later,
        // so there is no rotor here yet to annotate. POINT reads this list.
        if (!m_announcedDevices.contains(record.deviceName)) {
            m_announcedDevices.append(record.deviceName);
        }
        break;
    }
    case RecordType::Point: {
        // POINT is what proves a named device is a rotator with a heading, so
        // this is where the table gains entries.
        //
        // Deliberately NOT noteAuthoritativeState(). That gate is about the
        // switch panel's retained roster, and a heading says nothing about
        // where the relays are; tripping it here would let a rotator's first
        // POINT enable a pre-drop antenna grid.
        if (record.deviceName.isEmpty()) {
            return;
        }
        GreenHeronRotorState& rotor = m_rotors[record.deviceName];
        rotor.name        = record.deviceName;
        rotor.heading     = record.heading;
        rotor.status      = record.pointStatus;
        rotor.announced   = rotor.announced
                            || m_announcedDevices.contains(record.deviceName);
        rotor.lastPointMs = m_clock.elapsed();
        if (!m_rotorOrder.contains(record.deviceName)) {
            m_rotorOrder.append(record.deviceName);
        }
        if (!m_liveRotors.contains(record.deviceName)) {
            m_liveRotors.append(record.deviceName);
        }
        // Nothing arrives when a rotator goes quiet, so the only way to notice
        // is to look. Started on the first POINT rather than on connect, so a
        // server with no rotator never runs it at all.
        if (!m_rotorSilenceTimer->isActive()) {
            m_rotorSilenceTimer->start();
        }
        break;
    }
    case RecordType::Unknown:
        // The verbs handled here are not provably the whole vocabulary — the
        // captures behind them cover two installations. Log and carry on
        // rather than treating it as an error.
        qCDebug(lcDevices) << "GreenHeron: unhandled record" << record.verb;
        break;
    }
}

QStringList GreenHeronModel::displayOrder() const
{
    // Only switches whose SWITCHADD has landed. A SWITCHUPDATE or SWITCHLOCKS
    // that arrives first creates an entry with real state but no port roster,
    // and offering that as a choice would put a switch with no antennas on
    // screen. The entry itself is kept — the device may not resend that state —
    // it simply is not presentable until its ports are known.
    QStringList names;
    names.reserve(m_switches.size());
    for (auto it = m_switches.constBegin(); it != m_switches.constEnd(); ++it) {
        if (!it->ports.isEmpty()) {
            names.append(it.key());
        }
    }
    std::sort(names.begin(), names.end(), displayOrderLessThan);
    return names;
}

GreenHeronSwitchState GreenHeronModel::switchState(const QString& name) const
{
    return m_switches.value(name);
}

QMap<QString, QString> GreenHeronModel::locksBySwitch(const QString& name) const
{
    QMap<QString, QString> held;

    const auto it = m_switches.constFind(name);
    if (it == m_switches.constEnd() || it->locks.isEmpty()) {
        return held;
    }

    // m_announced, never displayOrder() — see the header comment. Using a
    // sorted basis here mislabels which switch holds an antenna, with no
    // crash and nothing logged.
    for (int slot = 0; slot < it->locks.size(); ++slot) {
        if (slot >= m_announced.size()) {
            break;
        }
        const QString& holder = m_announced.at(slot);
        if (holder == name) {
            continue;
        }
        const QString& port = it->locks.at(slot);
        if (port.isEmpty() || port == QLatin1String("OFF")) {
            continue;
        }
        held.insert(port, holder);
    }
    return held;
}

// ── Rotators ────────────────────────────────────────────────────────────────

void GreenHeronModel::onRotorSilenceTick()
{
    QStringList live;
    for (const QString& name : std::as_const(m_rotorOrder)) {
        if (isRotorLive(name)) {
            live.append(name);
        }
    }
    if (live == m_liveRotors) {
        return;
    }

    for (const QString& name : std::as_const(m_liveRotors)) {
        if (!live.contains(name)) {
            qCDebug(lcDevices) << "GreenHeron: rotator" << name
                               << "silent for" << kRotorSilentAfterMs
                               << "ms - treating it as gone";
        }
    }
    m_liveRotors = live;
    if (live.isEmpty()) {
        m_rotorSilenceTimer->stop();
    }
    emit panelChanged();
}

QStringList GreenHeronModel::rotorNames() const
{
    QStringList names;
    names.reserve(m_rotorOrder.size());
    for (const QString& name : m_rotorOrder) {
        if (isRotorLive(name)) {
            names.append(name);
        }
    }
    return names;
}

GreenHeronRotorState GreenHeronModel::rotorState(const QString& name) const
{
    return m_rotors.value(name);
}

bool GreenHeronModel::isRotorLive(const QString& name) const
{
    const auto it = m_rotors.constFind(name);
    if (it == m_rotors.constEnd()) {
        return false;
    }
    return (m_clock.elapsed() - it->lastPointMs) < kRotorSilentAfterMs;
}

bool GreenHeronModel::turnTo(const QString& rotorName, double degrees)
{
    if (m_socket == nullptr || !m_connected) {
        setLastError(tr("Not connected"));
        return false;
    }
    // Liveness, not the switch replay gate: m_rotors is emptied for every
    // socket we open, so a live rotator is by construction one that has
    // reported on THIS connection. That is the same guarantee isReady() gives
    // the antenna grid, established by the record that matters here.
    if (!isRotorLive(rotorName)) {
        setLastError(tr("\"%1\" is not reporting a heading").arg(rotorName));
        return false;
    }

    const QByteArray wire = encodeTurn(rotorName, degrees);
    if (wire.isEmpty()) {
        qCWarning(lcDevices) << "GreenHeron: refusing to send un-encodable turn"
                             << rotorName << degrees;
        setLastError(tr("Cannot turn to %1°: outside the 0–360° the device is "
                        "known to accept").arg(degrees, 0, 'f', kHeadingDecimals));
        return false;
    }

    qCDebug(lcDevices) << "GreenHeron: turn" << rotorName << "->" << degrees;
    m_socket->write(wire);
    // No local write-back, on purpose. A commanded 64.3 settled at a mean
    // 62.9 and dithered over several degrees; the reported heading arrives in
    // the next POINT and is the only one this model will ever hold.
    return true;
}

bool GreenHeronModel::selectPort(const QString& switchName, const QString& portName)
{
    // isReady(), not m_connected: a reconnect whose replay has not landed is
    // showing a pre-drop roster, and this call moves real relays.
    if (m_socket == nullptr || !isReady()) {
        setLastError(m_connected ? tr("Waiting for the switch to report its state")
                                 : tr("Not connected"));
        return false;
    }

    const QByteArray wire = encodeSelect(switchName, portName);
    if (wire.isEmpty()) {
        qCWarning(lcDevices) << "GreenHeron: refusing to send un-encodable selection"
                             << switchName << portName;
        setLastError(tr("Cannot select \"%1\": the name contains a framing character")
                         .arg(portName));
        return false;
    }

    qCDebug(lcDevices) << "GreenHeron: select" << switchName << "->" << portName;
    m_socket->write(wire);
    return true;
}

void GreenHeronModel::setLastError(const QString& message)
{
    m_lastError = message;
    emit errorOccurred(message);
}

} // namespace AetherSDR
