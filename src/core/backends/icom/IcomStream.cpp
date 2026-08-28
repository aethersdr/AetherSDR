#include "core/backends/icom/IcomStream.h"

#include <QAbstractSocket>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace AetherSDR::icom {

Q_LOGGING_CATEGORY(lcIcomStream, "aether.icom.stream")

namespace {

// How often the reorder buffer is examined. Well below kReorderHoldMs so a gap
// is noticed with time left to actually repair it.
constexpr int kReorderTickMs = 20;

// Past twice the hold window the packet is not coming back. Skipping forward
// beats stalling: on the audio stream a permanent stall is silence, and on the
// serial stream it is a radio that appears to stop answering.
constexpr int kGiveUpMs = kReorderHoldMs * 2;

std::span<const std::uint8_t> asSpan(const QByteArray& b)
{
    return {reinterpret_cast<const std::uint8_t*>(b.constData()), static_cast<std::size_t>(b.size())};
}

}  // namespace

IcomStream::IcomStream(QObject* parent) : QObject(parent) {}

IcomStream::~IcomStream() { stop(); }

bool IcomStream::start(const Config& config)
{
    if (!bindOnly(config))
        return false;
    beginHandshake();
    return true;
}

namespace {

// The three RS-BA1 streams fail independently and for different reasons, so a
// failure that does not say which one failed sends the operator to the wrong
// setting. Logged as a bare int() elsewhere in this file; a name costs nothing.
const char* roleName(IcomStream::Role r)
{
    switch (r) {
    case IcomStream::Role::Control: return "control";
    case IcomStream::Role::Serial:  return "CI-V";
    case IcomStream::Role::Audio:   return "audio";
    }
    return "?";
}

}  // namespace

bool IcomStream::bindOnly(const Config& config)
{
    stop();
    m_config = config;
    m_counters = Counters{};
    m_activityClock.start();
    m_lastRxAtMs = -1;
    m_lastTxAtMs = -1;
    m_lastPayloadAtMs = -1;
    m_lastPingReplyAtMs = -1;

    m_socket = new QUdpSocket(this);
    connect(m_socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                ++m_counters.socketErrors;
                m_counters.lastSocketError = m_socket
                    ? m_socket->errorString() : QStringLiteral("socket unavailable");
                qCWarning(lcIcomStream) << "UDP socket error on the"
                                        << roleName(m_config.role) << "stream:"
                                        << m_counters.lastSocketError;
            });
    if (!m_socket->bind(QHostAddress::AnyIPv4, config.localPort)) {
        if (m_counters.socketErrors == 0) {
            ++m_counters.socketErrors;
        }
        m_counters.lastSocketError = m_socket->errorString();
        emit failed(QStringLiteral("cannot bind local UDP port %1").arg(config.localPort));
        return false;
    }
    m_boundPort = m_socket->localPort();
    // connectToHost fixes the peer so writeDatagram/write both go to the radio
    // and, more usefully, makes the kernel drop datagrams from anywhere else.
    m_socket->connectToHost(config.host, config.remotePort);
    connect(m_socket, &QUdpSocket::readyRead, this, &IcomStream::onReadyRead);

    // A random high half rather than kappanhang's local-IP derivation: the ID
    // is opaque to the radio, and deriving it from the address puts the host's
    // LAN address into a payload that also carries an obfuscated password.
    m_localSid = deriveLocalSessionId(QRandomGenerator::global()->generate() & 0xffff, m_boundPort);
    m_remoteSid = 0;
    m_gotRemoteSid = false;
    m_ready = false;
    // ONE, NOT ZERO — and this is not cosmetic.
    //
    // The radio treats a tracked sequence of 0 as one BEFORE the start of the
    // space, infers a wrap, and concludes it has missed the entire preceding
    // window. Observed against a real radio: it answered our login with a
    // stream of retransmit requests for 0xff81..0xffff and never processed the
    // login itself, so the session hung after a handshake that had visibly
    // succeeded. kappanhang sets sendSeq = 1 in pkt0.init() for the same reason.
    m_txSeq = 1;
    m_pingSeq = 0;
    m_haveDelivered = false;
    m_gapPending = false;
    m_replay.clear();
    m_reorder.clear();
    m_idleSince.start();
    return true;
}

void IcomStream::beginHandshake()
{
    if (!m_socket)
        return;

    // RETRY, DO NOT JUST WAIT.
    //
    // This used to send AreYouThere twice and then sit on a 3 s deadline. Over
    // WiFi that is one shot: the pair goes out together, and if the radio is
    // mid-wake, momentarily off-channel, or still tearing down a session an
    // earlier client left behind, both are lost and the connect fails with a
    // message about Network Control that is simply wrong. It was the single
    // most common failure during bring-up and every time the fix was "try
    // again", which is exactly what a client should be doing for the operator.
    //
    // A second apart, because the failure being covered is a radio that is busy
    // for a moment rather than a lossy link — retrying faster than the radio
    // can answer just adds sessions for it to sort out.
    m_handshakeAttempts = 0;
    if (!m_handshakeTimer) {
        m_handshakeTimer = new QTimer(this);
        m_handshakeTimer->setInterval(kHandshakeRetryMs);
        connect(m_handshakeTimer, &QTimer::timeout, this, [this] {
            if (m_ready) {
                m_handshakeTimer->stop();
                return;
            }
            if (++m_handshakeAttempts >= kHandshakeAttempts) {
                m_handshakeTimer->stop();
                // The handshake has no explicit failure packet — a radio that is
                // off, busy with another client, or has Network Control disabled
                // simply says nothing. A deadline is the only way to report it,
                // and now it is a deadline the radio has been asked repeatedly
                // to beat.
                //
                // Port mismatch leads the causes because custom port triplets
                // shipped in #5230: the radio and this client can now disagree
                // about the port while both are configured and reachable, and it
                // is the one cause the old message never suggested. A raw probe
                // to the default port can even answer while the radio listens for
                // RS-BA1 somewhere else, which reads as proof the port is right.
                // The control stream is the one that proves the session-wide
                // conditions. Media streams only handshake from
                // openMediaStreams(), which is reached after m_authOk AND a
                // granted stream request -- so by then Network Control is
                // demonstrably on, the credentials are demonstrably good, and
                // nobody else holds the session. Repeating those causes for a
                // CI-V or audio timeout would point the operator at three
                // settings that are provably fine, which is the misdirection
                // this message exists to remove.
                const QString causes = (m_config.role == Role::Control)
                    ? QStringLiteral(
                        "Check the radio's Network menu: that its port for this "
                        "stream matches the one above, that Network Control is "
                        "ON, that the user/password are set, and that no other "
                        "client holds the session")
                    : QStringLiteral(
                        "The control stream connected, so Network Control and "
                        "the credentials are good — check that this stream's "
                        "port on the radio matches the one above");
                emit failed(QStringLiteral(
                    "no answer from %1:%2 (%3 stream) after %4 attempts — the "
                    "radio is not answering RS-BA1 on that port. %5")
                        .arg(m_config.host.toString())
                        .arg(m_config.remotePort)
                        .arg(QString::fromLatin1(roleName(m_config.role)))
                        .arg(kHandshakeAttempts)
                        .arg(causes));
                return;
            }
            // Name the target on every line. Six identical lines that do not say
            // where they were sent read as an AetherSDR networking fault; the
            // address and port are what turn them into a radio-side check.
            //
            // Deliberately still INF. A retry is not yet a failure -- a connect
            // that succeeds on attempt 2 is a normal connect, and warning on
            // each one would put up to 18 warnings on the log for a single
            // failed connect across three streams. The terminal failure is the
            // event worth the level, and IcomSession::fail already logs that at
            // qCWarning. What was missing here was the TARGET, not the severity.
            qCInfo(lcIcomStream)
                << "no IAmHere from"
                << QStringLiteral("%1:%2").arg(m_config.host.toString())
                                          .arg(m_config.remotePort)
                << "on the" << roleName(m_config.role) << "stream — retrying"
                << "AreYouThere, attempt" << (m_handshakeAttempts + 1)
                << "of" << kHandshakeAttempts;
            sendRawTwice(buildAreYouThere(m_localSid));
        });
    }
    sendRawTwice(buildAreYouThere(m_localSid));
    m_handshakeTimer->start();
}

void IcomStream::stop()
{
    if (m_socket && m_gotRemoteSid) {
        // Best-effort: a radio that never hears a disconnect holds the session
        // until it times out, and the operator cannot reconnect meanwhile.
        const auto bye = buildDisconnect(m_localSid, m_remoteSid);
        m_socket->write(reinterpret_cast<const char*>(bye.data()),
                        static_cast<qint64>(bye.size()));
        m_socket->write(reinterpret_cast<const char*>(bye.data()),
                        static_cast<qint64>(bye.size()));
        m_socket->flush();
    }
    for (QTimer** t : {&m_idleTimer, &m_pingTimer, &m_reorderTimer}) {
        if (*t) {
            (*t)->stop();
            (*t)->deleteLater();
            *t = nullptr;
        }
    }
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_ready = false;
    m_gotRemoteSid = false;
}

void IcomStream::sendRaw(std::span<const std::uint8_t> packet)
{
    if (!m_socket)
        return;
    const qint64 n = m_socket->write(reinterpret_cast<const char*>(packet.data()),
                                     static_cast<qint64>(packet.size()));
    if (n > 0) {
        m_counters.txBytes += static_cast<quint64>(n);
        ++m_counters.txPackets;
        m_lastTxAtMs = m_activityClock.elapsed();
    }
}

void IcomStream::flush()
{
    // sendRaw() only WRITES into QAbstractSocket's buffer. Without this the
    // bytes sit there until the event loop next runs — which is exactly what a
    // blocking wait prevents. See IcomSession::stop().
    if (m_socket)
        m_socket->flush();
}

void IcomStream::sendRawTwice(std::span<const std::uint8_t> packet)
{
    sendRaw(packet);
    sendRaw(packet);
}

void IcomStream::retain(quint16 seq, const std::vector<std::uint8_t>& packet)
{
    // FIFO BY INSERTION, not by key.
    //
    // m_replay is a QMap keyed by sequence, so erase(begin()) drops the
    // numerically-lowest key — which is the oldest packet only while the
    // sequence space has not wrapped. Once m_txSeq rolls past 0xFFFF the map
    // holds keys near 0xFFFF and near 0x0000 together, and the lowest key is
    // the FRESHEST packet: the buffer then evicts exactly the sequences most
    // likely to be asked for. A retransmit request for one of them finds
    // nothing and gets an Idle carrying that sequence instead of the payload —
    // a silently dropped CI-V command or audio frame. On the audio stream at
    // ~100 packets/s that comes round about every 11 minutes of transmit.
    //
    // This is the same wrap hazard onReorderTick() documents at length and
    // deliberately avoids; it bit here in the other direction.
    if (!m_replay.contains(seq))
        m_replayOrder.push_back(seq);
    m_replay.insert(seq, packet);
    while (m_replay.size() > kReplayDepth && !m_replayOrder.empty()) {
        m_replay.remove(m_replayOrder.front());
        m_replayOrder.pop_front();
    }
}

void IcomStream::sendTracked(std::vector<std::uint8_t> packet)
{
    sendTrackedImpl(std::move(packet), true);
}

void IcomStream::sendTrackedImpl(std::vector<std::uint8_t> packet, bool isPayload)
{
    if (packet.size() < kHeaderSize)
        return;
    const quint16 seq = m_txSeq++;
    // Stamp the header sequence here rather than trusting the caller: the
    // replay buffer is keyed by it, and a caller-allocated sequence could name
    // a packet this stream cannot reproduce — which presents as a retransmit
    // request that can never be satisfied and a stream stalled behind it.
    packet[0x06] = static_cast<std::uint8_t>(seq & 0xff);
    packet[0x07] = static_cast<std::uint8_t>((seq >> 8) & 0xff);
    sendRaw(packet);
    retain(seq, packet);
    // Only PAYLOAD resets the quiet clock. Letting the keepalive reset it would
    // make the stream permanently believe it had just sent something real, so
    // the relaxation to a 1 s cadence would never engage.
    if (isPayload)
        m_idleSince.restart();
}

void IcomStream::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray buf;
        buf.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
        const qint64 n = m_socket->readDatagram(buf.data(), buf.size());
        if (n <= 0)
            continue;
        buf.resize(static_cast<qsizetype>(n));
        m_counters.rxBytes += static_cast<quint64>(n);
        ++m_counters.rxPackets;
        m_lastRxAtMs = m_activityClock.elapsed();
        // EVERY inbound datagram, before any dispatch. On first contact with
        // real hardware the failure was a step that produced no reply at all,
        // and no amount of logging at the dispatch sites can distinguish "the
        // radio said nothing" from "we ignored what it said".
        if (!isPing(asSpan(buf))) {
            qCDebug(lcIcomStream) << "role" << roleName(m_config.role) << "RX" << buf.size()
                                  << "bytes:" << buf.left(64).toHex(' ');
        }
        handleDatagram(buf);
    }
}

void IcomStream::handleDatagram(const QByteArray& datagram)
{
    const auto pkt = asSpan(datagram);
    if (pkt.size() < kHeaderSize)
        return;

    // Pings first, and they never reach the layer above. The radio pings
    // frequently and a client that treats them as payload floods its own
    // decoder with 21-byte non-frames.
    if (isPing(pkt)) {
        if (auto p = parsePing(pkt)) {
            if (!p->isReply) {
                sendRaw(buildPingReply(m_localSid, m_remoteSid, *p));
            } else if (m_pingSentAt.isValid()) {
                const int rtt = static_cast<int>(m_pingSentAt.elapsed());
                // Smooth it: a single sample on WiFi swings enough to make the
                // readout unreadable, and this number is only ever displayed.
                m_counters.rttMs = m_counters.rttMs < 0 ? rtt : (m_counters.rttMs + rtt) / 2;
                m_lastPingReplyAtMs = m_activityClock.elapsed();
            }
        }
        return;
    }

    const Header h = parseHeader(pkt);

    if (!m_gotRemoteSid) {
        quint32 remote = 0;
        if (parseIAmHere(pkt, remote)) {
            m_remoteSid = remote;
            m_gotRemoteSid = true;
            qCInfo(lcIcomStream) << roleName(m_config.role) << "got i-am-here, remote sid"
                                 << Qt::hex << remote;
            sendRawTwice(buildAreYouReady(m_localSid, m_remoteSid));
        }
        return;
    }

    if (!m_ready) {
        if (isIAmReady(pkt)) {
            m_ready = true;
            if (m_handshakeTimer)
                m_handshakeTimer->stop();
            qCInfo(lcIcomStream) << roleName(m_config.role) << "handshake complete on local port"
                                 << m_boundPort;

            // NO PERIODIC IDLES ON THE AUDIO STREAM.
            //
            // kappanhang is explicit about this — "this stream does not use
            // periodic pkt0 idle packets" — and it matters more than it looks.
            // Our idles are TRACKED: each consumes a sequence number and enters
            // the replay buffer, so the radio can ask for any of them back. On
            // the audio stream that is 10 extra tracked packets a second layered
            // on top of 100 audio packets a second, competing for the same
            // airtime and the same sequence space as the audio it is meant to be
            // keeping alive. The pkt7 ping below is what actually holds the
            // stream open.
            if (m_config.role != Role::Audio) {
                m_idleTimer = new QTimer(this);
                connect(m_idleTimer, &QTimer::timeout, this, &IcomStream::onIdleTick);
                m_idleTimer->start(kIdleIntervalMs);
            }

            m_pingTimer = new QTimer(this);
            connect(m_pingTimer, &QTimer::timeout, this, &IcomStream::onPingTick);
            m_pingTimer->start(kPingIntervalMs);

            if (m_config.role != Role::Control) {
                m_reorderTimer = new QTimer(this);
                connect(m_reorderTimer, &QTimer::timeout, this, &IcomStream::onReorderTick);
                m_reorderTimer->start(kReorderTickMs);
            }
            emit ready();
        }
        return;
    }

    if (h.type == static_cast<quint16>(PacketType::Retransmit)) {
        handleRetransmitRequest(pkt);
        return;
    }
    if (h.type == static_cast<quint16>(PacketType::Disconnect)) {
        emit failed(QStringLiteral("the radio closed the session"));
        return;
    }
    if (h.type != static_cast<quint16>(PacketType::Idle))
        return;

    // A bare 16-byte idle is a keepalive, not payload — but it still occupies a
    // sequence number, so the reorder buffer has to see it or every keepalive
    // reads as a gap.
    if (m_config.role == Role::Control) {
        if (pkt.size() > kHeaderSize)
            deliver(datagram);
        return;
    }
    queueForReorder(h.seq, datagram);
}

void IcomStream::handleRetransmitRequest(std::span<const std::uint8_t> pkt)
{
    for (const SeqRange& r : parseRetransmitRequest(pkt)) {
        quint16 s = r.first;
        for (int i = 0; i < r.count() && i <= kMaxRetransmitRun; ++i, ++s) {
            auto it = m_replay.find(s);
            if (it != m_replay.end()) {
                sendRaw(*it);
                ++m_counters.retransmitsServed;
            } else {
                // We no longer hold it. Sending an IDLE carrying the requested
                // sequence number is what unblocks the radio's own reorder
                // buffer — staying silent stalls it, because it is waiting for
                // that number specifically and has no way to learn it is gone.
                sendRaw(buildIdle(m_localSid, m_remoteSid, s));
            }
        }
    }
}

void IcomStream::queueForReorder(quint16 seq, const QByteArray& packet)
{
    if (!m_haveDelivered) {
        m_haveDelivered = true;
        m_lastDelivered = seq;
        deliver(packet);
        return;
    }
    // Late or duplicate. This is not an error — it is a retransmission that
    // arrived after we gave up, and delivering it would put audio out of order.
    if (compareSeq(seq, m_lastDelivered) <= 0)
        return;
    m_reorder.insert(seq, packet);
    drainReorder();
}

void IcomStream::drainReorder()
{
    for (;;) {
        const quint16 next = static_cast<quint16>(m_lastDelivered + 1);
        auto it = m_reorder.find(next);
        if (it == m_reorder.end())
            break;
        const QByteArray packet = *it;
        m_reorder.erase(it);
        m_lastDelivered = next;
        deliver(packet);
    }
    m_gapPending = !m_reorder.isEmpty();
    if (m_gapPending && !m_headWait.isValid())
        m_headWait.start();
    if (!m_gapPending)
        m_headWait.invalidate();
}

void IcomStream::onReorderTick()
{
    if (m_reorder.isEmpty() || !m_headWait.isValid())
        return;

    const qint64 waited = m_headWait.elapsed();
    if (waited < kReorderHoldMs)
        return;

    const quint16 missing = static_cast<quint16>(m_lastDelivered + 1);
    // NOT m_reorder.firstKey(). QMap orders by numeric key, and the sequence
    // space WRAPS at 0xFFFF — so at a rollover the lowest key is the newest
    // packet, not the oldest, and the gap would compute as ~65000 and skip the
    // whole buffer. Scan for the smallest forward distance instead; the buffer
    // is bounded by the gap, so this is a handful of entries.
    quint16 haveUpTo = missing;
    quint16 bestDistance = 0xFFFF;
    for (auto it = m_reorder.constBegin(); it != m_reorder.constEnd(); ++it) {
        const quint16 distance = static_cast<quint16>(it.key() - missing);
        if (distance < bestDistance) {
            bestDistance = distance;
            haveUpTo = it.key();
        }
    }
    const int gap = static_cast<int>(bestDistance);

    if (waited < kGiveUpMs) {
        // Ask, once per tick, for the head of the gap. The single-sequence form
        // is used rather than the range form because the range form is
        // unverified against real hardware (see buildRetransmitRanges); more
        // datagrams, but provably correct.
        const int ask = std::min(gap, kMaxRetransmitRun);
        for (int i = 0; i < ask; ++i) {
            sendRaw(buildRetransmitRequest(m_localSid, m_remoteSid,
                                           static_cast<quint16>(missing + i)));
            ++m_counters.retransmitsAsked;
        }
        return;
    }

    // Give up and skip forward. A permanent stall is worse than a hole: on
    // audio it is silence forever, and on the serial stream it is a radio that
    // appears to have stopped answering.
    m_counters.rxLost += static_cast<quint64>(gap);
    emit packetsLost(gap);
    m_lastDelivered = static_cast<quint16>(haveUpTo - 1);
    m_headWait.invalidate();
    drainReorder();
}

void IcomStream::deliver(const QByteArray& packet)
{
    if (packet.size() > static_cast<qsizetype>(kHeaderSize)) {
        m_lastPayloadAtMs = m_activityClock.elapsed();
    }
    emit payloadReady(packet);
}

IcomStream::Counters IcomStream::counters() const
{
    Counters out = m_counters;
    if (!m_activityClock.isValid()) {
        return out;
    }
    const qint64 now = m_activityClock.elapsed();
    const auto age = [now](qint64 atMs) {
        return atMs >= 0 ? std::max<qint64>(0, now - atMs) : -1;
    };
    out.lastRxAgeMs = age(m_lastRxAtMs);
    out.lastTxAgeMs = age(m_lastTxAtMs);
    out.lastPayloadAgeMs = age(m_lastPayloadAtMs);
    out.lastPingReplyAgeMs = age(m_lastPingReplyAtMs);
    return out;
}

void IcomStream::onIdleTick()
{
    // The radio drops a stream that goes quiet. Relax to 1 s once nothing real
    // has been sent for a second — the difference between 10 and 1 wakeups a
    // second on an idle link, which matters on a battery-powered host.
    const bool quiet = m_idleSince.elapsed() > kIdleRelaxedAfterMs;
    const int want = quiet ? kIdleRelaxedIntervalMs : kIdleIntervalMs;
    if (m_idleTimer && m_idleTimer->interval() != want)
        m_idleTimer->setInterval(want);

    // Idles are TRACKED: they consume a sequence number, and the radio will ask
    // us to replay one exactly as it would a data packet. But they are NOT
    // payload, so they must not reset the quiet clock the relaxation reads.
    sendTrackedImpl(buildIdle(m_localSid, m_remoteSid, 0), false);
}

void IcomStream::onPingTick()
{
    std::array<std::uint8_t, 4> stamp{};
    const quint32 v = QRandomGenerator::global()->generate();
    stamp[0] = static_cast<std::uint8_t>(v & 0xff);
    stamp[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    stamp[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    stamp[3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    m_pingSentAt.start();
    sendRaw(buildPingRequest(m_localSid, m_remoteSid, m_pingSeq++, stamp));
}

}  // namespace AetherSDR::icom
