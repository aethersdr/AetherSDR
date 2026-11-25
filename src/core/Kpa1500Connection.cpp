#include "Kpa1500Connection.h"
#include "LogManager.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSignalBlocker>
#include <QStringList>

#include <algorithm>

namespace AetherSDR {

namespace {

// Polled every tick: the meter readings, temperature, and whether a tune is
// running, i.e. the values that move while the operator is transmitting.
const QStringList& fastPollCommands()
{
    static const QStringList kFast{
        QStringLiteral("PWF"),
        QStringLiteral("PWR"),
        QStringLiteral("SW"),
        QStringLiteral("TM"),
        QStringLiteral("TP"),
        QStringLiteral("PWI"),
        QStringLiteral("VI"),
    };
    return kFast;
}

// One of these per tick, round-robin. Configuration readbacks: they change
// only when the operator (or the radio's band data) changes them, so a full
// cycle of ~3.5 s on UDP and ~7 s on TCP is plenty (a control's own readback
// goes out with it, so a click does not wait for this). ^MA is skipped once
// known.
const QStringList& slowPollCommands()
{
    static const QStringList kSlow{
        QStringLiteral("FL"),
        QStringLiteral("OS"),
        QStringLiteral("BN"),
        QStringLiteral("AN"),
        QStringLiteral("AM"),
        QStringLiteral("AI"),
        QStringLiteral("ON"),
        QStringLiteral("FC"),
        QStringLiteral("FS"),
        QStringLiteral("AS"),
        QStringLiteral("FR"),
        QStringLiteral("DA"),
        QStringLiteral("WL"),
        QStringLiteral("MA"),
    };
    return kSlow;
}

// Asked once per link-up: identity, and what Wake-on-LAN needs (^MA, ^WL).
// ^RV and ^SN answer while the amp sleeps; ^MA and ^WL are asked again on
// the wake edge in case they do not.
const QStringList& identityCommands()
{
    static const QStringList kIdentity{
        QStringLiteral("RV"),
        QStringLiteral("SN"),
        QStringLiteral("MA"),
        QStringLiteral("WL"),
    };
    return kIdentity;
}

}  // namespace

Kpa1500Connection::Kpa1500Connection(QObject* parent)
    : QObject(parent)
{
    // Like UDP, a TCP link is up on the amp's first reply, not on the
    // handshake: a peer that accepts and never answers is a failed connect.
    connect(&m_socket, &QTcpSocket::connected, this, &Kpa1500Connection::onTcpOpen);
    connect(&m_socket, &QTcpSocket::disconnected, this, [this]() { onTransportDown(); });
    connect(&m_socket, &QTcpSocket::readyRead, this, &Kpa1500Connection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        // No answer at all fits an amp whose network sleeps; a refusal (one
        // TCP client already holds it) or an unknown name does not.
        onTransportError(m_socket.errorString(), err == QAbstractSocket::SocketTimeoutError
                                                     || err == QAbstractSocket::NetworkError);
    });

    // A connected QUdpSocket only accepts datagrams from the amp's address
    // and port. "Connected" here means the name resolved, not that the amp
    // answered: the link is declared up on the first well-formed reply.
    connect(&m_udp, &QUdpSocket::connected, this, [this]() {
        m_udpOpen = true;
        qCDebug(lcTuner) << "Kpa1500Connection: UDP socket open to" << description()
                         << "— probing for a reply";
        m_pollTimer.start();
        poll();
    });
    connect(&m_udp, &QUdpSocket::readyRead, this, &Kpa1500Connection::onUdpReadyRead);
    connect(&m_udp, &QUdpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        const QString text = m_udp.errorString();
        if (err == QAbstractSocket::HostNotFoundError) {
            ++m_connectEpoch;
            m_udp.abort();
            m_udpOpen = false;
            m_pollTimer.stop();
            onTransportError(text);
            return;
        }
        // ICMP port-unreachable and similar arrive here per datagram. The
        // silence timeout owns link state; this only feeds the error text.
        if (text != m_udpLastError) {
            qCDebug(lcTuner) << "Kpa1500Connection: UDP error" << text;
        }
        m_udpLastError = text;
    });

    m_parser.setMessageCallback([this](const Kpa1500::Message& m) { onMessage(m); });

    // Retries indefinitely until the amp returns or the user disconnects —
    // same precedent as every other peripheral connection here, for a
    // device that may legitimately be power-cycling.
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Kpa1500Connection::reconnect);

    m_wakeSender = [this](const QByteArray& packet, const QHostAddress& target, quint16 port) {
        return m_wakeSocket.writeDatagram(packet, target, port);
    };

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Kpa1500Connection::poll);

    m_clock.start();
    m_paceTimer.setSingleShot(true);
    connect(&m_paceTimer, &QTimer::timeout, this, &Kpa1500Connection::pumpQueries);

    m_trackTimer.setSingleShot(true);
    connect(&m_trackTimer, &QTimer::timeout, this, &Kpa1500Connection::sendTrack);

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
    // not be able to put anything on the air.
    if (m_keyed && m_connected) {
        writeFrame(Kpa1500::buildUnkey());
        m_socket.flush();
    }
}

QString Kpa1500Connection::description() const
{
    if (m_lastHost.isEmpty()) {
        return {};
    }
    QString text = QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
    if (m_activeTransport == Kpa1500::Transport::Udp) {
        text += QStringLiteral(" (UDP)");
    }
    return text;
}

bool Kpa1500Connection::linkOpen() const
{
    if (m_activeTransport == Kpa1500::Transport::Udp) {
        return m_udpOpen;
    }
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void Kpa1500Connection::reconnect()
{
    if (m_connected || m_lastHost.isEmpty()) { return; }
    connectNetwork(m_lastHost, m_lastPort);
}

void Kpa1500Connection::connectNetwork(const QString& host, quint16 port)
{
    m_sleepSentMs.reset();
    if (m_asleepOffline && (host != m_lastHost || port != m_lastPort)) {
        setAsleepOffline(false);   // another amplifier: the sleeping one is not ours to wake
    }
    if (host != m_lastHost) {
        m_peerAddress.clear();
        m_linkUpSeen = false;
    }
    m_lastHost = host;
    m_lastPort = port;
    m_activeTransport = m_transport;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    // Tearing down the previous link is part of this attempt, not a drop
    // that should arm the reconnect timer against it.
    m_deliberateDisconnect = true;
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_udp.abort();
    m_udpOpen = false;
    m_deliberateDisconnect = false;
    if (m_connected) {
        // A UDP link has no socket-level disconnected() to report this.
        m_connected = false;
        stopKeyRefresh();
        emit disconnected();
    }
    m_udpLastError.clear();
    m_pending.clear();
    m_pacer.clear();
    m_paceTimer.stop();
    m_pollDeferred = false;
    m_trackSentKhz.reset();
    m_trackTimer.stop();
    m_parser.reset();
    m_status = {};

    const bool udp = m_activeTransport == Kpa1500::Transport::Udp;
    m_pollTimer.setInterval(udp ? kUdpPollIntervalMs : kPollIntervalMs);

    const int epoch = ++m_connectEpoch;
    qCDebug(lcTuner) << "Kpa1500Connection: connecting to" << host << ":" << port
                     << "over" << Kpa1500::transportLabel(m_activeTransport);
    if (udp) {
        m_udp.connectToHost(host, port);
    } else {
        m_socket.connectToHost(host, port);
    }

    QTimer::singleShot(kConnectTimeoutMs, this, [this, epoch, udp]() {
        // Epoch guard: a timeout armed for attempt N must never abort
        // attempt N+1, which may already have succeeded (#2380).
        if (epoch != m_connectEpoch || m_connected) { return; }
        qCWarning(lcTuner) << "Kpa1500Connection: connect to" << description()
                           << "timed out after" << kConnectTimeoutMs << "ms";
        if (udp) {
            m_udp.abort();
            m_udpOpen = false;
            m_pollTimer.stop();
            onTransportError(m_udpLastError.isEmpty()
                ? tr("No reply from the amplifier on UDP port %1").arg(m_lastPort)
                : tr("No reply from the amplifier on UDP port %1 (%2)")
                      .arg(m_lastPort).arg(m_udpLastError), true);
            return;
        }
        const bool opened = m_socket.state() == QAbstractSocket::ConnectedState;
        {
            const QSignalBlocker quiet(m_socket);   // this attempt's failure is reported once, below
            m_socket.abort();
        }
        m_pollTimer.stop();
        onTransportError(opened ? tr("No reply from the amplifier on TCP port %1").arg(m_lastPort)
                                : tr("Connection timed out"), true);
    });
}

void Kpa1500Connection::disconnect()
{
    // Self-contained rather than relying on abort() to synchronously fire
    // disconnected() — a user-initiated disconnect that never emits
    // disconnected() leaves the applet stuck showing "Connected".
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    ++m_connectEpoch;  // a pending connect timeout must not resurrect a user disconnect
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    // Release the amp before dropping the link. Only ever an UNkey.
    if (m_keyed && wasConnected) {
        writeFrame(Kpa1500::buildUnkey());
        m_socket.flush();
    }
    stopKeyRefresh();
    m_connected = false;
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_udp.abort();
    m_udpOpen = false;
    m_pending.clear();
    m_pacer.clear();
    m_paceTimer.stop();
    m_pollDeferred = false;
    m_trackTimer.stop();
    m_trackSentKhz.reset();
    m_parser.reset();
    m_status = {};
    m_lastFaultCode = 0;
    m_wakeSentAt.invalidate();
    m_sleepSentMs.reset();
    m_linkUpSeen = false;
    if (wasConnected) {
        qCDebug(lcTuner) << "Kpa1500Connection: disconnected";
        emit disconnected();
    }
    setAsleepOffline(false);
    m_deliberateDisconnect = false;
}

void Kpa1500Connection::setAsleepOffline(bool on)
{
    if (m_asleepOffline == on) {
        return;
    }
    m_asleepOffline = on;
    if (!on) {
        m_sleepNetWake.reset();
        m_wakeSentAt.invalidate();
    }
    emit asleepOfflineChanged(on);
}

bool Kpa1500Connection::canWakeFromSleep() const
{
    // A MAC, and an IPv4 address to aim at: a host name with no link this
    // run has none (wakeTargets() would be empty).
    const QHostAddress host(m_lastHost);
    const bool target = host.protocol() == QAbstractSocket::IPv4Protocol
        || m_peerAddress.protocol() == QAbstractSocket::IPv4Protocol;
    return Kpa1500::parseMacAddress(m_wakeMac).has_value() && target;
}

void Kpa1500Connection::wakeFromSleep()
{
    if (!canWakeFromSleep()) {
        qCWarning(lcTuner) << "Kpa1500Connection: wake refused — no saved MAC address for Wake-on-LAN";
        return;
    }
    sendWakeOnLan(true);
    m_wakeSentAt.start();
    // A probing UDP link is still open; otherwise (TCP, or a UDP attempt
    // that timed out) dial again, and keep dialing for
    // kWakeReconnectWindowMs, while the amp boots.
    if (!linkOpen() && !m_reconnectTimer.isActive()) {
        reconnect();
    }
}

void Kpa1500Connection::onTcpOpen()
{
    qCDebug(lcTuner) << "Kpa1500Connection: TCP open to" << description() << "— waiting for a reply";
    m_slowPollIndex = 0;
    m_pollTimer.start();
    poll();
}

void Kpa1500Connection::onTransportUp()
{
    m_connected = true;
    m_linkUpSeen = true;
    m_sleepSentMs.reset();
    setAsleepOffline(false);
    m_peerAddress = m_activeTransport == Kpa1500::Transport::Udp ? m_udp.peerAddress()
                                                                 : m_socket.peerAddress();
    m_maAsks = 0;
    m_wlAsks = 0;
    ++m_connectEpoch;  // the connect timeout must not abort a later probe of this link
    m_lastFaultCode = 0;
    m_lastRxMs = m_clock.elapsed();
    m_trackSentKhz.reset();
    qCInfo(lcTuner) << "Kpa1500Connection: connected to" << description();
    // TRACK waits for a known power state (scheduleTrack), so ask for it now
    // rather than when the slow rotation next reaches ^ON.
    queueQuery(QStringLiteral("ON"), true);
    for (const QString& cmd : identityCommands()) {
        queueQuery(cmd);
    }
    // The probe poll that drew this reply (onTcpOpen, or UDP's) is in flight.
    if (!m_pollTimer.isActive()) {
        m_pollTimer.start();
    }
    emit connected();
    scheduleTrack();
}

void Kpa1500Connection::dropSilentLink()
{
    const bool udp = m_activeTransport == Kpa1500::Transport::Udp;
    qCWarning(lcTuner) << "Kpa1500Connection: no" << (udp ? "UDP" : "TCP") << "reply from"
                       << description() << "for" << kLinkSilenceMs << "ms — treating the link as down";
    if (udp) {
        onTransportDown(true);
        return;
    }
    // A peer that went quiet without FIN/RST leaves the socket connected
    // until the kernel gives up; close it and take the normal drop path once.
    {
        const QSignalBlocker quiet(m_socket);
        m_socket.abort();
    }
    onTransportDown();
}

bool Kpa1500Connection::sleepJustSent() const
{
    return m_sleepSentMs && m_clock.elapsed() - *m_sleepSentMs < kSleepDropWindowMs;
}

void Kpa1500Connection::onTransportDown(bool keepProbing)
{
    const bool wasConnected = m_connected;
    // Confirmed asleep when the link went: the amp's network may sleep too.
    // A sleep sent just now whose readback never came counts too: the amp's
    // network can stop the moment it takes ^ON0.
    const bool asleep = wasConnected && (m_status.powerOn == false || sleepJustSent());
    const std::optional<bool> netWake = m_status.netWake;
    m_connected = false;
    if (keepProbing) {
        // The next reply is a fresh link: nothing read on this one carries over.
        m_status = {};
        m_trackSentKhz.reset();
        m_lastFaultCode = 0;
    } else {
        m_pollTimer.stop();
        m_pacer.clear();
        m_paceTimer.stop();
        m_pollDeferred = false;
    }
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
    m_pending.clear();
    m_trackTimer.stop();
    m_parser.reset();
    if (asleep && !m_deliberateDisconnect) {
        m_sleepNetWake = netWake;
        m_asleepOffline = true;   // set before disconnected() so the owner keeps the applet up
    }
    if (wasConnected) {
        qCDebug(lcTuner) << "Kpa1500Connection: disconnected";
        emit disconnected();
    }
    if (m_asleepOffline && asleep) {
        qCInfo(lcTuner) << "Kpa1500Connection: link dropped while the amp sleeps — POWER can wake it";
        emit asleepOfflineChanged(true);
    }
    if (!keepProbing && !m_deliberateDisconnect && (m_autoReconnect || wakeReconnecting())) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

bool Kpa1500Connection::wakeReconnecting() const
{
    return m_asleepOffline && m_wakeSentAt.isValid() && m_wakeSentAt.elapsed() < kWakeReconnectWindowMs;
}

void Kpa1500Connection::onTransportError(const QString& errorString, bool mayBeAsleep)
{
    qCWarning(lcTuner) << "Kpa1500Connection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (mayBeAsleep && !m_deliberateDisconnect && !m_connected && !m_linkUpSeen && !m_asleepOffline
        && canWakeFromSleep()) {
        // No link, but a saved MAC: the amp may be asleep since before this
        // run. Offer POWER (Wake-on-LAN) in the applet's ASLEEP state.
        qCInfo(lcTuner) << "Kpa1500Connection: no link to" << description()
                        << "— offering Wake-on-LAN with the saved MAC";
        setAsleepOffline(true);
    }
    if (!m_deliberateDisconnect && (m_autoReconnect || wakeReconnecting()) && !m_connected) {
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
    onTcpBytes(m_socket.readAll());
}

void Kpa1500Connection::onTcpBytes(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    m_lastRxMs = m_clock.elapsed();
    if (!m_connected) {
        onTransportUp();
    }
    m_parser.feed(bytes);
}

void Kpa1500Connection::onUdpReadyRead()
{
    while (m_udp.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_udp.receiveDatagram(kMaxDatagramBytes);
        if (!datagram.isValid()) {
            continue;
        }
        // Each datagram is framed on its own: a lost datagram must not let
        // the tail of one reply splice onto the head of another.
        m_parser.reset();
        m_parser.feed(datagram.data());
        m_parser.reset();
    }
}

void Kpa1500Connection::onMessage(const Kpa1500::Message& message)
{
    if (m_activeTransport == Kpa1500::Transport::Udp) {
        if (!m_udpOpen) {
            return;
        }
        m_lastRxMs = m_clock.elapsed();
        const bool released = m_pacer.replied(message.cmd);
        if (!m_connected) {
            onTransportUp();
        }
        applyReply(message);
        // After the reply is applied: a deferred poll started from here must
        // see the state (and the confirmation) this reply carries.
        if (released) {
            pumpQueries();
        }
        return;
    }
    applyReply(message);
}

void Kpa1500Connection::applyReply(const Kpa1500::Message& message)
{
    const std::optional<bool> wasPowerOn = m_status.powerOn;
    const bool changed = Kpa1500::applyMessage(message, m_status);

    // The amp's own `^TQ` is authoritative for the key state: every reply
    // counts, repeated or not, and only a `^TQ` reply does.
    if (message.cmd == QLatin1String("TQ") && m_status.keyed && !*m_status.keyed && m_keyed) {
        qCWarning(lcTuner) << "Kpa1500Connection: amp reports not keyed — stopping the key refresh";
        stopKeyRefresh();
    }

    checkPendingControls(message);

    if (message.cmd == QLatin1String("ON") && m_status.powerOn == true && m_sleepSentMs
        && m_clock.elapsed() - *m_sleepSentMs > kPollIntervalMs) {
        m_sleepSentMs.reset();   // awake after the sleep: refused, or woken at the panel
    }

    if (!changed) {
        return;
    }

    if (message.cmd == QLatin1String("ON") && m_status.powerOn == false && wasPowerOn != false) {
        // Asleep: an unconfirmed ^FR would only be retried into a sleeping
        // amp and expire as "not confirmed". The wake below sends it again.
        m_trackTimer.stop();
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const PendingControl& p) {
                                           return p.group == QLatin1String("FR");
                                       }),
                        m_pending.end());
        m_trackSentKhz.reset();
    }
    if (message.cmd == QLatin1String("ON") && m_status.powerOn == true && wasPowerOn != true) {
        // The amp just woke, or reported awake for the first time on this
        // link: give it the frequency (again).
        m_trackSentKhz.reset();
        scheduleTrack();
        if (!m_status.macAddress) {
            queueQuery(QStringLiteral("MA"));
        }
        if (!m_status.netWake) {
            queueQuery(QStringLiteral("WL"));
        }
    }
    if (message.cmd == QLatin1String("MA") && m_status.macAddress) {
        m_wakeMac = *m_status.macAddress;
        emit macAddressReported(*m_status.macAddress);
    }

    const int fault = m_status.faultCode.value_or(0);
    if (fault != m_lastFaultCode) {
        m_lastFaultCode = fault;
        if (fault != 0) {
            qCWarning(lcTuner).noquote() << "Kpa1500Connection: amplifier fault"
                << QStringLiteral("%1").arg(fault, 2, 16, QLatin1Char('0')).toUpper();
        }
    }

    emit statusUpdated(m_status);
}

void Kpa1500Connection::poll()
{
    if (m_connected && m_clock.elapsed() - m_lastRxMs > kLinkSilenceMs) {
        dropSilentLink();
        return;
    }
    if (!linkOpen()) { return; }

    const bool udp = m_activeTransport == Kpa1500::Transport::Udp;

    if (udp && !m_pacer.drained()) {
        // The last cycle is still waiting on replies: start the next one when
        // it drains (pumpQueries), not on top of it.
        m_pollDeferred = true;
        retryPendingControls();
        return;
    }
    m_pollDeferred = false;

    // A sleeping amp answers only ^ON (and identity); polling the meters
    // would just be unanswered traffic until it wakes.
    if (m_status.powerOn == false) {
        queueQuery(QStringLiteral("ON"));
        retryPendingControls();
        return;
    }

    for (const QString& cmd : fastPollCommands()) {
        queueQuery(cmd);
    }
    if (m_keyed) {
        queueQuery(QStringLiteral("TQ"));
    }
    const QStringList& slow = slowPollCommands();
    // Firmware before 01.67 has no ^WL, and an amp may never answer ^MA:
    // stop spending pacer timeouts on a query unanswered kMaxWakeQueryAsks
    // times on this link. A known ^WL keeps being read (the front panel can
    // change it); a known ^MA is not.
    auto skip = [this](const QString& cmd) {
        if (cmd == QLatin1String("MA")) {
            return m_status.macAddress.has_value() || m_maAsks >= kMaxWakeQueryAsks;
        }
        if (cmd == QLatin1String("WL")) {
            return !m_status.netWake && m_wlAsks >= kMaxWakeQueryAsks;
        }
        return false;
    };
    for (int i = 0; i < slow.size() && skip(slow.at(m_slowPollIndex % slow.size())); ++i) {
        m_slowPollIndex = (m_slowPollIndex + 1) % slow.size();
    }
    const QString& slowCmd = slow.at(m_slowPollIndex % slow.size());
    if (slowCmd == QLatin1String("MA")) {
        ++m_maAsks;
    } else if (slowCmd == QLatin1String("WL") && !m_status.netWake) {
        ++m_wlAsks;
    }
    queueQuery(slowCmd);
    m_slowPollIndex = (m_slowPollIndex + 1) % slow.size();

    retryPendingControls();
}

void Kpa1500Connection::queueQuery(const QString& cmd, bool urgent)
{
    if (m_activeTransport != Kpa1500::Transport::Udp) {
        writeFrame(Kpa1500::buildQuery(cmd));
        return;
    }
    m_pacer.enqueue(cmd, urgent);
    pumpQueries();
}

void Kpa1500Connection::pumpQueries()
{
    if (m_activeTransport != Kpa1500::Transport::Udp) {
        return;
    }
    if (!linkOpen()) {
        // Closed under us (connect timeout, host not found): nothing to wait for.
        m_pacer.clear();
        m_paceTimer.stop();
        m_pollDeferred = false;
        return;
    }
    while (linkOpen()) {
        const std::optional<QString> next = m_pacer.takeNext(m_clock.elapsed());
        if (!next) {
            break;
        }
        if (writeFrame(Kpa1500::buildQuery(*next))) {
            break;  // one query outstanding until its reply or the timeout
        }
        m_pacer.replied(*next);  // nothing went out, so nothing to wait for
    }
    const qint64 wait = m_pacer.msUntilTimeout(m_clock.elapsed());
    if (wait >= 0) {
        m_paceTimer.start(static_cast<int>(wait) + 1);
    } else {
        m_paceTimer.stop();
    }
    if (m_pacer.drained() && m_pollDeferred) {
        m_pollDeferred = false;
        poll();
    }
}

bool Kpa1500Connection::writeFrame(const QByteArray& frame)
{
    if (frame.isEmpty() || !linkOpen()) {
        return false;
    }
    if (m_activeTransport == Kpa1500::Transport::Udp) {
        return m_udp.write(frame) == frame.size();
    }
    return m_socket.write(frame) == frame.size();
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
    return writeFrame(frame);
}

bool Kpa1500Connection::sendControl(PendingControl control)
{
    // The antenna and ATU in-line/bypass relays must never switch under RF,
    // including the unkey tail (the TX flag drops at UNKEY_REQUESTED). A
    // refused click is dropped, not queued: the operator clicks again.
    // (A control already pending when TX began is held by
    // retryPendingControls() instead.)
    if (m_connected && (m_transmitting || inUnkeySettle())
        && (control.group == QLatin1String("AN") || control.group == QLatin1String("AI"))) {
        qCWarning(lcTuner) << "Kpa1500Connection: refusing" << control.label << "while transmitting";
        emit controlRefused(tr("%1 refused while transmitting").arg(control.label));
        return false;
    }
    if (!sendRaw(control.frame)) {
        return false;
    }
    if (m_activeTransport != Kpa1500::Transport::Udp) {
        // SETs get no reply; ask now so the applet shows the result at once
        // rather than when the slow rotation reaches it.
        writeFrame(Kpa1500::buildQuery(control.readback));
        return true;
    }
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&control](const PendingControl& p) {
                                       return p.group == control.group;
                                   }),
                    m_pending.end());
    queueQuery(control.readback, true);
    control.attempts = 1;
    control.lastSent.start();
    m_pending.push_back(std::move(control));
    return true;
}

void Kpa1500Connection::checkPendingControls(const Kpa1500::Message& message)
{
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [this, &message](const PendingControl& p) {
                                       if (!p.satisfied(message, m_status)) {
                                           return false;
                                       }
                                       qCDebug(lcTuner) << "Kpa1500Connection:" << p.label
                                                        << "confirmed after" << p.attempts
                                                        << "attempt(s)";
                                       return true;
                                   }),
                    m_pending.end());
}

void Kpa1500Connection::retryPendingControls()
{
    QStringList expired;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->lastSent.elapsed() < kControlRetryMs) {
            ++it;
            continue;
        }
        if (m_transmitting && it->resend && !it->retryUnderTx) {
            // Never repeat a relay control under RF; resume once transmit ends.
            ++it;
            continue;
        }
        if (it->attempts >= kMaxControlAttempts) {
            qCWarning(lcTuner) << "Kpa1500Connection:" << it->label << "not confirmed after"
                               << it->attempts << "attempts";
            expired.append(it->label);
            it = m_pending.erase(it);
            continue;
        }
        if (it->resend) {
            writeFrame(it->frame);
        }
        queueQuery(it->readback, true);
        ++it->attempts;
        it->lastSent.start();
        ++it;
    }
    for (const QString& label : expired) {
        emit controlUnconfirmed(label);
    }
}

Kpa1500Connection::PendingControl Kpa1500Connection::operateControl(bool operate)
{
    PendingControl c;
    c.group = QStringLiteral("OS");
    c.label = operate ? tr("Operate") : tr("Standby");
    c.frame = Kpa1500::buildSetOperate(operate);
    c.readback = QStringLiteral("OS");
    c.retryUnderTx = !operate;
    c.satisfied = [operate](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("OS") && s.operate == operate;
    };
    return c;
}

void Kpa1500Connection::setOperate(bool operate)
{
    sendControl(operateControl(operate));
}

void Kpa1500Connection::startTune()
{
    // Never resent: a repeated ^FT could restart a tune already running.
    // Confirmed by ^TP1, or by the amp's ^FT; echo when a quick tune ends.
    PendingControl c;
    c.group = QStringLiteral("TUNE");
    c.label = tr("ATU tune");
    c.frame = Kpa1500::buildStartTune();
    c.readback = QStringLiteral("TP");
    c.resend = false;
    c.satisfied = [](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FT")
            || (m.cmd == QLatin1String("TP") && s.tuning == true);
    };
    sendControl(std::move(c));
}

Kpa1500Connection::PendingControl Kpa1500Connection::cancelTuneControl()
{
    PendingControl c;
    c.group = QStringLiteral("TUNE");
    c.label = tr("Tune cancel");
    c.frame = Kpa1500::buildCancelTune();
    c.readback = QStringLiteral("TP");
    c.retryUnderTx = true;
    c.satisfied = [](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FT")
            || (m.cmd == QLatin1String("TP") && s.tuning == false);
    };
    return c;
}

void Kpa1500Connection::cancelTune()
{
    sendControl(cancelTuneControl());
}

Kpa1500Connection::PendingControl Kpa1500Connection::atuInlineControl(bool inLine)
{
    PendingControl c;
    c.group = QStringLiteral("AI");
    c.label = inLine ? tr("ATU in-line") : tr("ATU bypass");
    c.frame = Kpa1500::buildSetAtuInline(inLine);
    c.readback = QStringLiteral("AI");
    c.satisfied = [inLine](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("AI") && s.atuInline == inLine;
    };
    return c;
}

void Kpa1500Connection::setAtuInline(bool inLine)
{
    sendControl(atuInlineControl(inLine));
}

void Kpa1500Connection::selectAntenna(int port)
{
    if (port < Kpa1500::kMinAntenna || port > Kpa1500::kMaxAntenna) {
        qCWarning(lcTuner) << "Kpa1500Connection: antenna port" << port
                           << "out of range — ignoring";
        return;
    }
    PendingControl c;
    c.group = QStringLiteral("AN");
    c.label = tr("Antenna %1").arg(port);
    c.frame = Kpa1500::buildSelectAntenna(port);
    c.readback = QStringLiteral("AN");
    c.satisfied = [port](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("AN") && s.antenna == port;
    };
    sendControl(std::move(c));
}

Kpa1500Connection::PendingControl Kpa1500Connection::clearFaultControl()
{
    PendingControl c;
    c.group = QStringLiteral("FL");
    c.label = tr("Fault clear");
    c.frame = Kpa1500::buildClearFault();
    c.readback = QStringLiteral("FL");
    c.retryUnderTx = true;
    c.satisfied = [](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FL") && s.faultCode == 0;
    };
    return c;
}

void Kpa1500Connection::clearFault()
{
    sendControl(clearFaultControl());
}

void Kpa1500Connection::setFanMinimum(int level)
{
    if (level < Kpa1500::kMinFan || level > Kpa1500::kMaxFan) {
        qCWarning(lcTuner) << "Kpa1500Connection: fan level" << level << "out of range — ignoring";
        return;
    }
    PendingControl c;
    c.group = QStringLiteral("FC");
    c.label = tr("Fan minimum %1").arg(level);
    c.frame = Kpa1500::buildSetFanMinimum(level);
    c.readback = QStringLiteral("FC");
    c.satisfied = [level](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FC") && s.fanMinimum == level;
    };
    sendControl(std::move(c));
}

void Kpa1500Connection::setPower(bool on)
{
    if (on && m_asleepOffline) {
        wakeFromSleep();
        return;
    }
    if (!on && m_transmitting) {
        qCWarning(lcTuner) << "Kpa1500Connection: refusing sleep while transmitting";
        emit controlRefused(tr("Sleep refused while transmitting"));
        return;
    }
    if (on && m_connected) {
        sendWakeOnLan();   // offline, only the asleep-offline state above wakes
    }
    PendingControl c;
    c.group = QStringLiteral("ON");
    c.label = on ? tr("Power on") : tr("Power off");
    c.frame = Kpa1500::buildSetPower(on);
    c.readback = QStringLiteral("ON");
    c.satisfied = [on](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("ON") && s.powerOn == on;
    };
    if (sendControl(std::move(c)) && !on) {
        m_sleepSentMs = m_clock.elapsed();
    } else if (on) {
        m_sleepSentMs.reset();
    }
}

void Kpa1500Connection::setFollowTx(bool on)
{
    if (m_followTx == on) {
        return;
    }
    m_followTx = on;
    m_trackSentKhz.reset();
    if (on) {
        scheduleTrack();
    } else {
        m_trackTimer.stop();
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const PendingControl& p) {
                                           return p.group == QLatin1String("FR");
                                       }),
                        m_pending.end());
    }
}

void Kpa1500Connection::setTxFrequency(std::optional<int> khz, bool localTransmitting,
                                     bool radioTransmitting)
{
    const bool transmitting = localTransmitting || radioTransmitting;
    const bool txEnded = m_transmitting && !transmitting;
    const bool txStarted = !m_transmitting && transmitting;
    m_transmitting = transmitting;
    if (txEnded) {
        // Give the radio's unkey sequencing a full retry interval before a
        // held control repeats, or a held ^FR goes out (scheduleTrack):
        // m_transmitting drops at UNKEY_REQUESTED (and at an optimistic local
        // MOX release), while RF may still be up.
        m_txEndedAt.start();
        for (auto& p : m_pending) {
            if (p.resend && !p.retryUnderTx) {
                p.lastSent.start();
            }
        }
    }
    if (txStarted) {
        // A ^FR still awaiting confirmation must not be resent under RF;
        // forget it and send it again once transmit ends.
        const auto before = m_pending.size();
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const PendingControl& p) {
                                           return p.group == QLatin1String("FR");
                                       }),
                        m_pending.end());
        if (m_pending.size() != before) {
            m_trackSentKhz.reset();
        }
        m_trackTimer.stop();
    }
    if (khz && (*khz < Kpa1500::kMinTrackKhz || *khz > Kpa1500::kMaxTrackKhz)) {
        khz.reset();  // a slice outside the amp's coverage (e.g. 2 m) is not tracked
    }
    if (!khz) {
        m_trackTimer.stop();
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const PendingControl& p) {
                                           return p.group == QLatin1String("FR");
                                       }),
                        m_pending.end());
        m_trackWantKhz.reset();
        m_trackSentKhz.reset();
        return;
    }
    if (khz == m_trackWantKhz && !txEnded) {
        return;
    }
    m_trackWantKhz = khz;
    scheduleTrack();
}

void Kpa1500Connection::scheduleTrack()
{
    // Track only an amp known to be awake: an unknown power state waits for
    // the first ^ON reading, which re-schedules this (onMessage).
    if (!m_followTx || !m_connected || m_transmitting || !m_trackWantKhz
        || m_trackWantKhz == m_trackSentKhz || m_status.powerOn != true) {
        return;
    }
    const qint64 wait = trackWaitMs();
    if (wait <= 0) {
        sendTrack();
        return;
    }
    if (!m_trackTimer.isActive()) {
        m_trackTimer.start(static_cast<int>(wait));
    }
}

bool Kpa1500Connection::inUnkeySettle() const
{
    return m_txEndedAt.isValid() && m_txEndedAt.elapsed() < kControlRetryMs;
}

qint64 Kpa1500Connection::trackWaitMs() const
{
    qint64 wait = 0;
    if (m_trackLastSent.isValid()) {
        wait = std::max(wait, kTrackIntervalMs - m_trackLastSent.elapsed());
    }
    if (m_txEndedAt.isValid()) {
        wait = std::max(wait, kControlRetryMs - m_txEndedAt.elapsed());
    }
    return wait;
}

void Kpa1500Connection::sendTrack()
{
    // Re-check: transmit may have started, tracking stopped, or the amp gone
    // to sleep, since the timer was armed.
    if (!m_followTx || !m_connected || m_transmitting || !m_trackWantKhz
        || m_trackWantKhz == m_trackSentKhz || m_status.powerOn != true) {
        return;
    }
    if (trackWaitMs() > 0) {
        scheduleTrack();   // e.g. a timer armed before the unkey settle began
        return;
    }
    const int khz = *m_trackWantKhz;
    PendingControl c;
    c.group = QStringLiteral("FR");
    c.label = tr("Frequency %1 kHz").arg(khz);
    c.frame = Kpa1500::buildSetFrequency(khz);
    c.readback = QStringLiteral("FR");
    c.satisfied = [khz](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FR") && s.frequencyKhz == khz;
    };
    m_trackLastSent.start();
    const QString label = c.label;
    if (!sendControl(std::move(c))) {
        // Not on the wire, so not sent: try again after the rate limit
        // rather than leaving the amp on its old band until the next QSY,
        // but give up (and say so) after as many tries as a control gets.
        if (++m_trackWriteFailures < kMaxControlAttempts) {
            m_trackTimer.start(kTrackIntervalMs);
        } else {
            qCWarning(lcTuner) << "Kpa1500Connection:" << label << "could not be written after"
                               << m_trackWriteFailures << "attempts";
            m_trackWriteFailures = 0;
            emit controlUnconfirmed(label);
        }
        return;
    }
    m_trackWriteFailures = 0;
    m_trackSentKhz = khz;
}

QList<QHostAddress> Kpa1500Connection::wakeTargets(const QHostAddress& amp,
                                                  const QList<QNetworkAddressEntry>& localEntries)
{
    QList<QHostAddress> targets;
    if (amp.protocol() != QAbstractSocket::IPv4Protocol) {
        return targets;
    }
    for (const QNetworkAddressEntry& entry : localEntries) {
        if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.broadcast().isNull()
            || !amp.isInSubnet(entry.ip(), entry.prefixLength()) || targets.contains(entry.broadcast())) {
            continue;
        }
        targets.append(entry.broadcast());
    }
    targets.append(amp);
    return targets;
}

void Kpa1500Connection::sendWakeOnLan(bool ignoreNetWake)
{
    if (!ignoreNetWake && m_status.netWake == false) {
        qCInfo(lcTuner) << "Kpa1500Connection: NET WAKE is off on the amp — no Wake-on-LAN packet";
        return;
    }
    const std::optional<QByteArray> mac =
        Kpa1500::parseMacAddress(m_status.macAddress.value_or(m_wakeMac));
    if (!mac) {
        qCInfo(lcTuner) << "Kpa1500Connection: MAC address not known — no Wake-on-LAN packet";
        return;
    }
    QHostAddress amp(m_lastHost);
    if (amp.isNull()) {
        amp = m_activeTransport == Kpa1500::Transport::Udp ? m_udp.peerAddress() : m_socket.peerAddress();
    }
    if (amp.isNull()) {
        amp = m_peerAddress;
    }
    QList<QNetworkAddressEntry> entries;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::IsRunning)
            && !flags.testFlag(QNetworkInterface::IsLoopBack)) {
            entries.append(iface.addressEntries());
        }
    }
    const QList<QHostAddress> targets = wakeTargets(amp, entries);
    if (targets.isEmpty()) {
        qCWarning(lcTuner) << "Kpa1500Connection: no IPv4 address for" << m_lastHost
                           << "— no Wake-on-LAN packet";
        return;
    }
    const QByteArray packet = Kpa1500::buildWakeOnLanPacket(*mac);
    QStringList sent;
    for (const QHostAddress& target : targets) {
        if (m_wakeSender(packet, target, Kpa1500::kWakeOnLanPort) == packet.size()) {
            sent << target.toString();
        } else {
            qCWarning(lcTuner) << "Kpa1500Connection: Wake-on-LAN to" << target.toString()
                               << "failed:" << m_wakeSocket.errorString();
        }
    }
    if (!sent.isEmpty()) {
        qCInfo(lcTuner).noquote() << "Kpa1500Connection: Wake-on-LAN for"
                                  << Kpa1500::formatMacAddress(*mac) << "sent to"
                                  << sent.join(QStringLiteral(", "));
    }
}

void Kpa1500Connection::key()
{
    if (!m_connected) {
        qCWarning(lcTuner) << "Kpa1500Connection: key() refused — not connected";
        return;
    }
    if (m_activeTransport == Kpa1500::Transport::Udp) {
        qCWarning(lcTuner) << "Kpa1500Connection: key() refused — network keying needs TCP";
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
