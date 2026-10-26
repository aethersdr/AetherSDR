#include "Kpa1500Connection.h"
#include "LogManager.h"

#include <QNetworkDatagram>
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
// only when the operator (or the radio's band data) changes them, so a
// ~3.5s full cycle is plenty and it keeps the wire quiet.
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
    };
    return kSlow;
}

// Asked once per connection: identity that does not change while connected.
// Both answer while the amp sleeps, which is also what lets a sleeping amp's
// link come up over UDP.
const QStringList& identityCommands()
{
    static const QStringList kIdentity{
        QStringLiteral("RV"),
        QStringLiteral("SN"),
    };
    return kIdentity;
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
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected || m_lastHost.isEmpty()) { return; }
        connectNetwork(m_lastHost, m_lastPort);
    });

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Kpa1500Connection::poll);

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

void Kpa1500Connection::connectNetwork(const QString& host, quint16 port)
{
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
        if (!udp && m_socket.state() == QAbstractSocket::ConnectedState) { return; }
        qCWarning(lcTuner) << "Kpa1500Connection: connect to" << description()
                           << "timed out after" << kConnectTimeoutMs << "ms";
        if (udp) {
            m_udp.abort();
            m_udpOpen = false;
            m_pollTimer.stop();
            onTransportError(m_udpLastError.isEmpty()
                ? tr("No reply from the amplifier on UDP port %1").arg(m_lastPort)
                : tr("No reply from the amplifier on UDP port %1 (%2)")
                      .arg(m_lastPort).arg(m_udpLastError));
            return;
        }
        m_socket.abort();
        onTransportError(tr("Connection timed out"));
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
    m_trackTimer.stop();
    m_trackSentKhz.reset();
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
    m_lastRx.start();
    m_trackSentKhz.reset();
    qCInfo(lcTuner) << "Kpa1500Connection: connected to" << description();
    m_pollTimer.start();
    for (const QString& cmd : identityCommands()) {
        writeFrame(Kpa1500::buildQuery(cmd));
    }
    poll();  // don't make the applet wait a full interval for its first reading
    emit connected();
    scheduleTrack();
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
    m_pending.clear();
    m_trackTimer.stop();
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
        m_lastRx.start();
        if (!m_connected) {
            onTransportUp();
        }
    }

    const std::optional<bool> wasPowerOn = m_status.powerOn;
    const bool changed = Kpa1500::applyMessage(message, m_status);

    // The amp's own `^TQ` is authoritative for the key state: every reply
    // counts, repeated or not, and only a `^TQ` reply does.
    if (message.cmd == QLatin1String("TQ") && m_status.keyed && !*m_status.keyed && m_keyed) {
        qCWarning(lcTuner) << "Kpa1500Connection: amp reports not keyed — stopping the key refresh";
        stopKeyRefresh();
    }

    checkPendingControls(message);

    if (!changed) {
        return;
    }

    if (message.cmd == QLatin1String("ON") && m_status.powerOn == true && wasPowerOn == false) {
        m_trackSentKhz.reset();  // the amp just woke: give it the frequency again
        scheduleTrack();
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
    if (!linkOpen()) { return; }

    if (m_activeTransport == Kpa1500::Transport::Udp && m_connected
        && m_lastRx.elapsed() > kUdpSilenceMs) {
        qCWarning(lcTuner) << "Kpa1500Connection: no UDP reply from" << description()
                           << "for" << kUdpSilenceMs << "ms — treating the link as down";
        m_udp.abort();
        m_udpOpen = false;
        onTransportDown();
        return;
    }

    // A sleeping amp answers only ^ON (and identity); polling the meters
    // would just be unanswered traffic until it wakes.
    if (m_status.powerOn == false) {
        writeFrame(Kpa1500::buildQuery(QStringLiteral("ON")));
        retryPendingControls();
        return;
    }

    for (const QString& cmd : fastPollCommands()) {
        writeFrame(Kpa1500::buildQuery(cmd));
    }
    if (m_keyed) {
        writeFrame(Kpa1500::buildKeyStateQuery());
    }
    const QStringList& slow = slowPollCommands();
    if (!slow.isEmpty()) {
        writeFrame(Kpa1500::buildQuery(slow.at(m_slowPollIndex % slow.size())));
        m_slowPollIndex = (m_slowPollIndex + 1) % slow.size();
    }

    retryPendingControls();
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

void Kpa1500Connection::sendControl(PendingControl control)
{
    if (!sendRaw(control.frame)) {
        return;
    }
    if (m_activeTransport != Kpa1500::Transport::Udp) {
        return;
    }
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&control](const PendingControl& p) {
                                       return p.group == control.group;
                                   }),
                    m_pending.end());
    writeFrame(Kpa1500::buildQuery(control.readback));
    control.attempts = 1;
    control.lastSent.start();
    m_pending.push_back(std::move(control));
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
        writeFrame(Kpa1500::buildQuery(it->readback));
        ++it->attempts;
        it->lastSent.start();
        ++it;
    }
    for (const QString& label : expired) {
        emit controlUnconfirmed(label);
    }
}

void Kpa1500Connection::setOperate(bool operate)
{
    PendingControl c;
    c.group = QStringLiteral("OS");
    c.label = operate ? tr("Operate") : tr("Standby");
    c.frame = Kpa1500::buildSetOperate(operate);
    c.readback = QStringLiteral("OS");
    c.satisfied = [operate](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("OS") && s.operate == operate;
    };
    sendControl(std::move(c));
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

void Kpa1500Connection::cancelTune()
{
    PendingControl c;
    c.group = QStringLiteral("TUNE");
    c.label = tr("Tune cancel");
    c.frame = Kpa1500::buildCancelTune();
    c.readback = QStringLiteral("TP");
    c.satisfied = [](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FT")
            || (m.cmd == QLatin1String("TP") && s.tuning == false);
    };
    sendControl(std::move(c));
}

void Kpa1500Connection::setAtuInline(bool inLine)
{
    PendingControl c;
    c.group = QStringLiteral("AI");
    c.label = inLine ? tr("ATU in-line") : tr("ATU bypass");
    c.frame = Kpa1500::buildSetAtuInline(inLine);
    c.readback = QStringLiteral("AI");
    c.satisfied = [inLine](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("AI") && s.atuInline == inLine;
    };
    sendControl(std::move(c));
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

void Kpa1500Connection::clearFault()
{
    PendingControl c;
    c.group = QStringLiteral("FL");
    c.label = tr("Fault clear");
    c.frame = Kpa1500::buildClearFault();
    c.readback = QStringLiteral("FL");
    c.satisfied = [](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("FL") && s.faultCode == 0;
    };
    sendControl(std::move(c));
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
    PendingControl c;
    c.group = QStringLiteral("ON");
    c.label = on ? tr("Power on") : tr("Power off");
    c.frame = Kpa1500::buildSetPower(on);
    c.readback = QStringLiteral("ON");
    c.satisfied = [on](const Kpa1500::Message& m, const Kpa1500::Status& s) {
        return m.cmd == QLatin1String("ON") && s.powerOn == on;
    };
    sendControl(std::move(c));
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

void Kpa1500Connection::setTxFrequency(std::optional<int> khz, bool transmitting)
{
    const bool txEnded = m_transmitting && !transmitting;
    const bool txStarted = !m_transmitting && transmitting;
    m_transmitting = transmitting;
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
    if (khz == m_trackWantKhz && !txEnded) {
        return;
    }
    m_trackWantKhz = khz;
    scheduleTrack();
}

void Kpa1500Connection::scheduleTrack()
{
    if (!m_followTx || !m_connected || m_transmitting || !m_trackWantKhz
        || m_trackWantKhz == m_trackSentKhz || m_status.powerOn == false) {
        return;
    }
    if (!m_trackLastSent.isValid() || m_trackLastSent.elapsed() >= kTrackIntervalMs) {
        sendTrack();
        return;
    }
    if (!m_trackTimer.isActive()) {
        m_trackTimer.start(static_cast<int>(kTrackIntervalMs - m_trackLastSent.elapsed()));
    }
}

void Kpa1500Connection::sendTrack()
{
    // Re-check: transmit may have started, or tracking stopped, since the
    // timer was armed.
    if (!m_followTx || !m_connected || m_transmitting || !m_trackWantKhz
        || m_trackWantKhz == m_trackSentKhz) {
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
    m_trackSentKhz = khz;
    m_trackLastSent.start();
    sendControl(std::move(c));
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
