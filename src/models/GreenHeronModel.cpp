#include "models/GreenHeronModel.h"

#include "core/LogManager.h"

#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

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
    m_pending.clear();
    m_connected = false;
    m_stale = false;
    m_lastError.clear();

    emit connectionStateChanged();
    emit panelChanged();
}

void GreenHeronModel::openSocket()
{
    teardownSocket();

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
    m_stale = false;
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

    const bool wasConnected = m_connected;
    m_connected = false;
    m_stale = !m_switches.isEmpty();
    m_keepAliveTimer->stop();
    m_connectTimer->stop();
    emit connectionStateChanged();

    // disconnected() does not always follow an error (a refused connect never
    // reached ESTABLISHED), so the retry is scheduled from here too; both
    // paths are idempotent through the timer's isActive() guard.
    if (!wasConnected) {
        scheduleReconnect();
    }
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

    for (const QByteArray& raw : records) {
        applyRecord(parse(raw));
    }
    emit panelChanged();
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

void GreenHeronModel::applyRecord(const Record& record)
{
    if (record.switchName.isEmpty()) {
        return;
    }

    switch (record.type) {
    case RecordType::SwitchAdd: {
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
        GreenHeronSwitchState& state = m_switches[record.switchName];
        state.name = record.switchName;
        state.selected = record.selected;
        state.wirelessSignal = record.wirelessSignal;
        break;
    }
    case RecordType::SwitchLocks: {
        GreenHeronSwitchState& state = m_switches[record.switchName];
        state.name = record.switchName;
        state.locks = record.locks;
        break;
    }
    case RecordType::Unknown:
        // The three verbs handled here are not provably the whole vocabulary —
        // the captures behind them cover one installation with one kind of
        // switch attached. Log and carry on rather than treating it as an
        // error.
        qCDebug(lcDevices) << "GreenHeron: unhandled record" << record.verb;
        break;
    }
}

QStringList GreenHeronModel::displayOrder() const
{
    QStringList names = m_switches.keys();
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

bool GreenHeronModel::selectPort(const QString& switchName, const QString& portName)
{
    if (m_socket == nullptr || !m_connected) {
        setLastError(tr("Not connected"));
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
