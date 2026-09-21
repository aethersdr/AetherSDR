#include "Kpa1500Connection.h"
#include "LogManager.h"

#include <QStringList>

namespace AetherSDR {

namespace {

// Polled every tick — the three meter readings plus temperature, i.e. the
// values that move while the operator is transmitting.
const QStringList& fastPollCommands()
{
    static const QStringList kFast{
        QStringLiteral("PWF"),
        QStringLiteral("PWR"),
        QStringLiteral("SW"),
        QStringLiteral("TM"),
    };
    return kFast;
}

// One of these per tick, round-robin. Configuration readbacks: they change
// only when the operator (or the radio's band data) changes them, so a
// ~3.5s full cycle is plenty and it keeps the wire quiet.
const QStringList& slowPollCommands()
{
    static const QStringList kSlow{
        QStringLiteral("FL"),
        QStringLiteral("OS"),
        QStringLiteral("ON"),
        QStringLiteral("BN"),
        QStringLiteral("AN"),
        QStringLiteral("AE"),
        QStringLiteral("AM"),
        QStringLiteral("AI"),
    };
    return kSlow;
}

}  // namespace

Kpa1500Connection::Kpa1500Connection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &Kpa1500Connection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &Kpa1500Connection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &Kpa1500Connection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    m_parser.setMessageCallback([this](const Kpa1500::Message& m) { onMessage(m); });

    // Retries indefinitely until the amp returns or the user disconnects —
    // same precedent as every other peripheral connection here, for a
    // device that may legitimately be power-cycling.
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected || m_lastHost.isEmpty()) { return; }
        connectNetwork(m_lastHost, m_lastPort);
    });

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Kpa1500Connection::poll);

    m_keyRefreshTimer.setInterval(kKeyRefreshMs);
    connect(&m_keyRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_keyed) { return; }
        sendRaw(Kpa1500::buildKey(kKeyTimeoutSec));
    });
}

Kpa1500Connection::~Kpa1500Connection()
{
    // Release the amp on the way out rather than leaving it to time out.
    // Guarded on m_keyed so this is a no-op on the overwhelmingly common
    // path (never keyed), and it can only ever UNkey — a destructor must
    // not be able to put anything on the air (Principle VI).
    if (m_keyed && m_connected) {
        m_socket.write(Kpa1500::buildUnkey());
        m_socket.flush();
    }
}

QString Kpa1500Connection::description() const
{
    if (m_lastHost.isEmpty()) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
}

void Kpa1500Connection::connectNetwork(const QString& host, quint16 port)
{
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_parser.reset();
    m_status = {};

    const int epoch = ++m_connectEpoch;
    qCDebug(lcTuner) << "Kpa1500Connection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);

    QTimer::singleShot(kConnectTimeoutMs, this, [this, epoch]() {
        // Epoch guard: a timeout armed for attempt N must never abort
        // attempt N+1, which may already have succeeded (#2380).
        if (epoch != m_connectEpoch || m_connected) { return; }
        if (m_socket.state() == QAbstractSocket::ConnectedState) { return; }
        qCWarning(lcTuner) << "Kpa1500Connection: connect to" << description()
                           << "timed out after" << kConnectTimeoutMs << "ms";
        m_socket.abort();
        onTransportError(tr("Connection timed out"));
    });
}

void Kpa1500Connection::disconnect()
{
    // Self-contained rather than relying on abort() to synchronously fire
    // disconnected() — the ACOM row learned that the hard way (see
    // AcomConnection::disconnect()): a user-initiated disconnect that never
    // emits disconnected() leaves the applet stuck showing "Connected".
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    // Release the amp before dropping the link. Only ever an UNkey.
    if (m_keyed && wasConnected) {
        m_socket.write(Kpa1500::buildUnkey());
        m_socket.flush();
    }
    stopKeyRefresh();
    m_connected = false;
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_parser.reset();
    m_status = {};
    m_lastFaultCode = 0;
    if (wasConnected) {
        qCDebug(lcTuner) << "Kpa1500Connection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void Kpa1500Connection::onTransportUp()
{
    m_connected = true;
    m_lastFaultCode = 0;
    m_slowPollIndex = 0;
    qCInfo(lcTuner) << "Kpa1500Connection: connected to" << description();
    m_pollTimer.start();
    poll();  // don't make the applet wait a full interval for its first reading
    emit connected();
}

void Kpa1500Connection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_pollTimer.stop();
    // The link is gone, so the key can no longer be refreshed OR released.
    // Clearing the flag here is what makes the amp's own `^TX` timeout the
    // fail-safe rather than something this class is still pretending to
    // manage — see the header's keying note.
    if (m_keyed) {
        qCWarning(lcTuner) << "Kpa1500Connection: link dropped while keyed — the amp's"
                              " own ^TX timeout (" << kKeyTimeoutSec << "s) is now the"
                              " only thing that will release it.";
    }
    stopKeyRefresh();
    m_parser.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "Kpa1500Connection: disconnected";
        emit disconnected();
    }
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void Kpa1500Connection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "Kpa1500Connection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
        armReconnect();
    }
}

void Kpa1500Connection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void Kpa1500Connection::onReadyRead()
{
    m_parser.feed(m_socket.readAll());
}

void Kpa1500Connection::onMessage(const Kpa1500::Message& message)
{
    if (!Kpa1500::applyMessage(message, m_status)) {
        return;
    }

    if (m_status.keyed) {
        // The amp's own view of its key state (`^TQ`) is authoritative —
        // if it says it is not keyed, this class does not get to keep
        // claiming it is (Principle II).
        m_keyed = *m_status.keyed;
        if (!m_keyed) {
            stopKeyRefresh();
        }
    }

    const int fault = m_status.faultCode.value_or(0);
    if (fault != m_lastFaultCode) {
        m_lastFaultCode = fault;
        if (fault != 0) {
            qCWarning(lcTuner) << "Kpa1500Connection: amplifier fault code" << fault;
            emit faultRaised(fault);
        } else {
            emit faultCleared();
        }
    }

    emit statusUpdated(m_status);
}

void Kpa1500Connection::poll()
{
    if (!m_connected) { return; }
    for (const QString& cmd : fastPollCommands()) {
        sendRaw(Kpa1500::buildQuery(cmd));
    }
    const QStringList& slow = slowPollCommands();
    if (!slow.isEmpty()) {
        sendRaw(Kpa1500::buildQuery(slow.at(m_slowPollIndex % slow.size())));
        m_slowPollIndex = (m_slowPollIndex + 1) % slow.size();
    }
}

bool Kpa1500Connection::sendRaw(const QByteArray& frame)
{
    if (frame.isEmpty()) {
        // A builder refused to produce a frame (out-of-range argument).
        // Never silently swallowed — same stance as VkampConnection.
        qCWarning(lcTuner) << "Kpa1500Connection: refusing to send an empty/invalid frame";
        return false;
    }
    if (!m_connected) {
        qCDebug(lcTuner) << "Kpa1500Connection: dropping command, not connected:" << frame;
        return false;
    }
    return m_socket.write(frame) == frame.size();
}

void Kpa1500Connection::setOperate(bool operate)
{
    sendRaw(Kpa1500::buildSetOperate(operate));
}

void Kpa1500Connection::setPower(bool on)
{
    sendRaw(Kpa1500::buildSetPower(on));
}

void Kpa1500Connection::startTune()
{
    sendRaw(Kpa1500::buildStartTune());
}

void Kpa1500Connection::cancelTune()
{
    sendRaw(Kpa1500::buildCancelTune());
}

void Kpa1500Connection::setAtuMode(Kpa1500::AtuMode mode)
{
    sendRaw(Kpa1500::buildSetAtuMode(mode));
}

void Kpa1500Connection::setAtuInline(bool inLine)
{
    sendRaw(Kpa1500::buildSetAtuInline(inLine));
}

void Kpa1500Connection::selectAntenna(int port)
{
    if (port < Kpa1500::kMinAntenna || port > Kpa1500::kMaxAntenna) {
        qCWarning(lcTuner) << "Kpa1500Connection: antenna port" << port
                           << "out of range — ignoring";
        return;
    }
    sendRaw(Kpa1500::buildSelectAntenna(port));
}

void Kpa1500Connection::clearFault()
{
    sendRaw(Kpa1500::buildClearFault());
}

void Kpa1500Connection::key()
{
    if (!m_connected) {
        qCWarning(lcTuner) << "Kpa1500Connection: key() refused — not connected";
        return;
    }
    if (!sendRaw(Kpa1500::buildKey(kKeyTimeoutSec))) {
        // Fail closed: if the key frame did not make it onto the wire, do
        // NOT start refreshing a key state we have no evidence of.
        qCWarning(lcTuner) << "Kpa1500Connection: key() write failed — not arming refresh";
        return;
    }
    m_keyed = true;
    m_keyRefreshTimer.start();
}

void Kpa1500Connection::unkey()
{
    // Unconditional, and never gated on m_keyed: releasing an amp that
    // this class believes is already released is harmless, while skipping
    // the release because of a stale flag is not.
    stopKeyRefresh();
    m_keyed = false;
    sendRaw(Kpa1500::buildUnkey());
}

void Kpa1500Connection::stopKeyRefresh()
{
    m_keyRefreshTimer.stop();
    m_keyed = false;
}

}  // namespace AetherSDR
