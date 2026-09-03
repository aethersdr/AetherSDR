#include "core/backends/hl2/Hl2TelemetryPoller.h"

#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

namespace AetherSDR::hl2 {

Hl2TelemetryPoller::Hl2TelemetryPoller(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::CoarseTimer);   // nothing here needs millisecond accuracy
    connect(m_timer, &QTimer::timeout, this, &Hl2TelemetryPoller::onPollTimer);
}

Hl2TelemetryPoller::~Hl2TelemetryPoller() = default;

void Hl2TelemetryPoller::setTarget(const QHostAddress& addr)
{
    if (m_target == addr)
        return;
    m_target = addr;
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
    const int interval = m_target.isNull() ? 0 : currentIntervalMs();

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
    }

    m_timer->start(interval);
    onPollTimer();   // do not make a stalled stream wait a full interval
}

void Hl2TelemetryPoller::onPollTimer()
{
    if (!m_socket || m_target.isNull())
        return;

    // A poll that went unanswered is a fact about the radio, and it has to be
    // counted at SEND time rather than on a timeout, because the absence of a
    // reply produces no event to hang a counter off. onReadyRead clears it.
    if (m_sinceRequest.isValid()) {
        ++m_unanswered;
        emit pollUnanswered(m_unanswered);
    }

    const auto pkt = discoveryRequest();
    m_socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                            static_cast<qint64>(pkt.size()),
                            m_target, kAltPort);
    m_sinceRequest.restart();
}

void Hl2TelemetryPoller::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        // Only the radio we asked. An unsolicited datagram from elsewhere on the
        // subnet is not telemetry about this radio, and rendering it as such
        // would be worse than showing nothing.
        if (dg.senderAddress() != m_target)
            continue;

        const QByteArray data = dg.data();
        const auto reply = parseDiscoveryReply(
            {reinterpret_cast<const std::uint8_t*>(data.constData()),
             static_cast<std::size_t>(data.size())});
        if (!reply || !reply->isHermesLite2())
            continue;

        const qint64 age = m_sinceRequest.isValid() ? m_sinceRequest.elapsed() : 0;
        m_sinceRequest.invalidate();
        m_unanswered = 0;
        emit readingReceived(*reply, age);
    }
}

}  // namespace AetherSDR::hl2
