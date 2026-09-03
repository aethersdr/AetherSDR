#include "core/backends/hl2/Hl2TelemetryPoller.h"

#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR::hl2 {

namespace {

// QUdpSocket does not enable SO_BROADCAST itself. Lifted from Hl2Discovery.cpp
// rather than shared, for the same reason AnanDiscovery duplicates it: three
// lines of platform glue behind a header is a worse trade than three lines
// repeated, and the alternative is a utility header that exists for one call.
void enableBroadcast(QUdpSocket& s) noexcept
{
    const qintptr fd = s.socketDescriptor();
    if (fd < 0)
        return;
    const int on = 1;
#ifdef Q_OS_WIN
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
#else
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
}

}  // namespace

Hl2TelemetryPoller::Hl2TelemetryPoller(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::CoarseTimer);   // nothing here needs millisecond accuracy
    connect(m_timer, &QTimer::timeout, this, &Hl2TelemetryPoller::onPollTimer);
}

Hl2TelemetryPoller::~Hl2TelemetryPoller() = default;

void Hl2TelemetryPoller::setAllowBroadcastFallback(bool allow)
{
    m_allowBroadcast = allow;
}

void Hl2TelemetryPoller::setExpectedMac(const std::array<std::uint8_t, 6>& mac)
{
    m_expectedMac = mac;
}

void Hl2TelemetryPoller::setTarget(const QHostAddress& addr)
{
    if (m_target == addr)
        return;
    m_target = addr;
    m_lastResponder = QHostAddress();
    // A new radio's counters are not the old radio's. Anything a consumer is
    // showing belongs to the previous target until the next reply arrives.
    m_unanswered = 0;
    applyCadence();
}

void Hl2TelemetryPoller::setLinkState(LinkState s)
{
    if (m_state == s)
        return;
    m_state = s;
    // Crossing into or out of Streaming changes who owns the readings, not just
    // how often we ask. Reset the silence count so a stall that begins right
    // after a healthy stream does not inherit a stale streak.
    m_unanswered = 0;
    applyCadence();
}

void Hl2TelemetryPoller::setSurfaceVisible(bool visible)
{
    if (m_surfaceVisible == visible)
        return;
    m_surfaceVisible = visible;
    applyCadence();
}

int Hl2TelemetryPoller::currentIntervalMs() const noexcept
{
    // The rule itself is in Hl2TelemetryCadence.h and is pinned by
    // hl2_telemetry_cadence_test. This class does not restate it.
    return hl2PollIntervalMs(m_state, m_surfaceVisible);
}

void Hl2TelemetryPoller::applyCadence()
{
    // A null target is not a reason to stop -- it selects broadcast. Only the
    // cadence rule decides whether to poll.
    const int interval = currentIntervalMs();

    if (interval <= 0) {
        m_timer->stop();
        // Drop the socket rather than leaving it bound. A poller that is not
        // polling should hold no resource and, more to the point, should not be
        // able to answer a question about a radio it stopped watching.
        if (m_socket) {
            m_socket->deleteLater();
            m_socket = nullptr;
        }
        return;
    }

    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        // Ephemeral local port, unbound: the reply comes back to whatever source
        // port we sent from, and the gateware records that per-packet for 1025
        // (network.v:686-698). Deliberately NOT the socket MetisClient uses —
        // the whole point of this class is to keep working when that one's
        // stream has stopped, and sharing its socket would tie the instrument to
        // the thing it is measuring.
        connect(m_socket, &QUdpSocket::readyRead, this, &Hl2TelemetryPoller::onReadyRead);
        // Bind before setting SO_BROADCAST: the option goes on a real
        // descriptor, and an unbound QUdpSocket has none yet (socketDescriptor()
        // returns -1 and the call silently does nothing). Hl2Discovery makes the
        // same ordering explicit for the same reason.
        m_socket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);
        enableBroadcast(*m_socket);
    }

    m_timer->start(interval);
    onPollTimer();   // do not make a stalled stream wait a full interval
}

void Hl2TelemetryPoller::onPollTimer()
{
    if (!m_socket)
        return;

    // WHERE TO SEND IS DECIDED FIRST, because the answer may be NOWHERE and the
    // bookkeeping below must not run for a poll that never happened.
    //
    // Unicast once a radio is known -- from setTarget(), from connectRadio(),
    // or from whichever one answered a previous broadcast. With no target and
    // no broadcast opt-in, send nothing: a broadcast reaches the LOCAL SEGMENT,
    // which is not necessarily where the radio is and may be where something
    // that must not be polled is. See setTarget.
    QHostAddress dest;
    if (!m_target.isNull())              dest = m_target;
    else if (!m_lastResponder.isNull())  dest = m_lastResponder;
    else if (m_allowBroadcast)           dest = QHostAddress::Broadcast;
    else {
        // Declining to send also retires any outstanding request: otherwise the
        // next real poll would inherit a pending one and report an unanswered
        // count for a datagram that was never on the wire.
        m_sinceRequest.invalidate();
        return;
    }

    // A poll that went unanswered is a fact about the radio, and it has to be
    // counted at SEND time rather than on a timeout, because the absence of a
    // reply produces no event to hang a counter off. onReadyRead clears it.
    //
    // Counted only for polls actually sent. A count rising while nothing left
    // the socket would say "the radio is not answering" about a radio nobody
    // asked -- precisely the misreading that let a broadcast to the wrong
    // segment look like a working no-reply case.
    if (m_sinceRequest.isValid()) {
        ++m_unanswered;
        emit pollUnanswered(m_unanswered);
    }

    const auto pkt = discoveryRequest();
    m_socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                            static_cast<qint64>(pkt.size()),
                            dest, kAltPort);
    m_sinceRequest.restart();
}

void Hl2TelemetryPoller::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        // When a target is set, only that radio. A datagram from elsewhere on
        // the subnet is not telemetry about this radio, and rendering it as
        // such would be worse than showing nothing.
        if (!m_target.isNull() && dg.senderAddress() != m_target)
            continue;

        const QByteArray data = dg.data();
        const auto reply = parseDiscoveryReply(
            {reinterpret_cast<const std::uint8_t*>(data.constData()),
             static_cast<std::size_t>(data.size())});
        if (!reply || !reply->isHermesLite2())
            continue;
        // With no target set, a broadcast can be answered by more than one
        // radio. Accepting the first is right for a single-radio bench and
        // wrong the moment there are two, so a caller that knows which radio it
        // means sets the serial and this drops the rest.
        if (m_expectedMac && reply->mac != *m_expectedMac)
            continue;
        m_lastResponder = dg.senderAddress();

        const qint64 age = m_sinceRequest.isValid() ? m_sinceRequest.elapsed() : 0;
        m_sinceRequest.invalidate();
        m_unanswered = 0;
        emit readingReceived(*reply, age);
    }
}

}  // namespace AetherSDR::hl2
