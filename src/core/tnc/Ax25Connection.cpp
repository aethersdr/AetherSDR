#include "core/tnc/Ax25Connection.h"

#include "core/LogManager.h"

#include <QTimer>

#include <algorithm>

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;
using ax25::FrameType;

Ax25Connection::Ax25Connection(QObject* parent)
    : QObject(parent)
{
    m_t1 = new QTimer(this);
    m_t1->setSingleShot(true);
    connect(m_t1, &QTimer::timeout, this, &Ax25Connection::onT1Timeout);

    m_t2 = new QTimer(this);
    m_t2->setSingleShot(true);
    connect(m_t2, &QTimer::timeout, this, &Ax25Connection::onT2Timeout);

    m_t3 = new QTimer(this);
    m_t3->setSingleShot(true);
    connect(m_t3, &QTimer::timeout, this, &Ax25Connection::onT3Timeout);

    m_clock.start();
}

void Ax25Connection::applyRecommendedTimers()
{
    m_t1Ms = ax25::recommendedT1Ms(m_profile, m_paclen);
    m_t2Ms = ax25::recommendedT2Ms(m_t1Ms);
    m_t3Ms = ax25::recommendedT3Ms(m_t1Ms);
}

Ax25Connection::~Ax25Connection() = default;

void Ax25Connection::setLocalAddress(const Address& local)
{
    m_primary = local;
    if (m_state == State::Disconnected)
        m_local = local; // idle: match/answer on the primary until a caller dials
}

int Ax25Connection::outstanding() const
{
    return (m_vs - m_va + 8) % 8;
}

void Ax25Connection::startT1()
{
    // T1 and T3 are mutually exclusive by construction: T1 means "something is
    // outstanding, I am waiting for it", T3 means "nothing is outstanding, is
    // the peer still alive". Polling an idle link while frames are in flight
    // would just add traffic to a link that is already waiting.
    stopT3();
    m_t1->start(m_t1Ms);
}

void Ax25Connection::stopT1()
{
    m_t1->stop();
}

void Ax25Connection::stopT3()
{
    if (m_t3)
        m_t3->stop();
}

bool Ax25Connection::idlePollArmed() const
{
    return m_t3 && m_t3->isActive();
}

void Ax25Connection::armT3IfIdle()
{
    if (m_state == State::Connected && outstanding() == 0 && !m_t1->isActive())
        m_t3->start(m_t3Ms);
    else
        stopT3();
}

void Ax25Connection::onT3Timeout()
{
    if (m_state != State::Connected || outstanding() != 0)
        return;
    // One checkpoint poll. We do NOT declare the link dead here — we hand the
    // question to T1/N2, which already knows how to retry a poll and give up.
    ++m_stats.t3Polls;
    emit activity(QStringLiteral("Idle-link check (T3 %1s): polling %2")
        .arg(m_t3Ms / 1000).arg(m_remote.toString()));
    qCInfo(lcAx25Link).noquote()
        << QStringLiteral("link idle-poll peer=%1 t3Ms=%2 idleForMs=%3")
               .arg(m_remote.toString())
               .arg(m_t3Ms)
               .arg(sessionDurationMs());
    sendSupervisory(FrameType::RR, /*pollFinal=*/true, /*command=*/true);
    startT1();
}

bool Ax25Connection::isDuplicateIFrame(int ns) const
{
    // Split the mod-8 sequence ring into "ahead of V(R)" (a real gap — we
    // missed something and the peer has moved on) and "behind V(R)" (a frame we
    // already took). The halfway split is the standard rule and holds for any
    // window size, so it stays correct if MAXFRAME ever rises above 1.
    const int ahead = (ns - m_vr + 8) % 8;
    return ahead >= 4;
}

void Ax25Connection::noteLinkProgress()
{
    m_retryCount = 0;
    m_rejRecoveries = 0;
}

qint64 Ax25Connection::sessionDurationMs() const
{
    if (m_sessionStartMs == 0)
        return 0;
    return m_clock.elapsed() - m_sessionStartMs;
}

void Ax25Connection::recordRttSample(int seq)
{
    // Karn's algorithm: a retransmitted frame's ack cannot be attributed to a
    // particular copy, so it tells us nothing about the true round trip. Using
    // it anyway would drag the estimate toward T1 and hide a mis-sized timer.
    // -1 means "no send stamp for this slot". A stamp of 0 is a real value (the
    // first frame of a session can land on elapsed()==0), and a round trip of
    // 0 ms is real too on a loopback or a same-tick test path, so neither may be
    // used as the sentinel.
    if (m_iFrameResent[seq] || m_iFrameSentMs[seq] < 0)
        return;

    const qint64 rtt = m_clock.elapsed() - m_iFrameSentMs[seq];
    if (rtt < 0)
        return;

    m_stats.rttLastMs = rtt;
    m_stats.rttSumMs += rtt;
    if (m_stats.rttSamples == 0) {
        m_stats.rttMinMs = rtt;
        m_stats.rttMaxMs = rtt;
    } else {
        m_stats.rttMinMs = std::min(m_stats.rttMinMs, rtt);
        m_stats.rttMaxMs = std::max(m_stats.rttMaxMs, rtt);
    }
    ++m_stats.rttSamples;

    qCInfo(lcAx25Link).noquote()
        << QStringLiteral("link rtt peer=%1 ns=%2 rttMs=%3 t1Ms=%4 headroomPct=%5 "
                          "avgRttMs=%6 samples=%7")
               .arg(m_remote.toString())
               .arg(seq)
               .arg(rtt)
               .arg(m_t1Ms)
               .arg(m_t1Ms > 0 ? (100 * (m_t1Ms - rtt)) / m_t1Ms : 0)
               .arg(m_stats.averageRttMs())
               .arg(m_stats.rttSamples);
}

QString Ax25Connection::sessionSummary() const
{
    const qint64 seconds = std::max<qint64>(1, sessionDurationMs() / 1000);
    const quint32 totalSent = m_stats.iSent + m_stats.iResent;
    return QStringLiteral(
        "durationS=%1 txBytes=%2 rxBytes=%3 throughputBps=%4 iSent=%5 iResent=%6 "
        "resentPct=%7 iRcvd=%8 iDropped=%9 iDuplicate=%19 rejSent=%10 rejRcvd=%11 "
        "t1Timeouts=%12 t3Polls=%13 rttMinMs=%14 rttAvgMs=%15 rttMaxMs=%16 samples=%17 "
        "t1Ms=%18")
        .arg(seconds)
        .arg(m_stats.infoBytesSent)
        .arg(m_stats.infoBytesReceived)
        .arg((m_stats.infoBytesSent + m_stats.infoBytesReceived) / seconds)
        .arg(m_stats.iSent)
        .arg(m_stats.iResent)
        .arg(totalSent > 0 ? (100 * m_stats.iResent) / totalSent : 0)
        .arg(m_stats.iRcvd)
        .arg(m_stats.iDropped)
        .arg(m_stats.rejSent)
        .arg(m_stats.rejRcvd)
        .arg(m_stats.t1Timeouts)
        .arg(m_stats.t3Polls)
        .arg(m_stats.rttMinMs)
        .arg(m_stats.averageRttMs())
        .arg(m_stats.rttMaxMs)
        .arg(m_stats.rttSamples)
        .arg(m_t1Ms)
        .arg(m_stats.iDuplicate);
}

void Ax25Connection::logSessionOpen()
{
    // The whole timing contract in one line. When a link then misbehaves, this
    // says immediately whether the settings were even physically plausible —
    // the failure that motivated all of this was a T1 shorter than our own
    // transmission, which no amount of channel analysis would have revealed.
    qCInfo(lcAx25Link).noquote()
        << QStringLiteral("link up peer=%1 local=%2 baud=%3 preambleFlags=%4 paclen=%5 "
                          "n2=%6 window=%7 t1Ms=%8 t2Ms=%9 t3Ms=%10 "
                          "modelIFrameMs=%11 modelRttMs=%12 via=%13")
               .arg(m_remote.toString(), m_local.toString())
               .arg(m_profile.baud)
               .arg(m_profile.preambleFlags)
               .arg(m_paclen)
               .arg(m_n2)
               .arg(m_window)
               .arg(m_t1Ms)
               .arg(m_t2Ms)
               .arg(m_t3Ms)
               .arg(expectedIFrameAirtimeMs())
               .arg(expectedRoundTripMs())
               .arg(m_via.isEmpty() ? QStringLiteral("direct")
                                    : QString::number(m_via.size()) + QStringLiteral(" hop(s)"));

    if (m_t1Ms < expectedRoundTripMs()) {
        // Impossible by construction, not merely tight: we cannot receive the
        // ack before T1 fires, so every frame will retransmit.
        qCWarning(lcAx25Link).noquote()
            << QStringLiteral("link T1_BELOW_MODEL peer=%1 t1Ms=%2 modelRttMs=%3 — "
                              "every I-frame will time out before the ack can arrive")
                   .arg(m_remote.toString())
                   .arg(m_t1Ms)
                   .arg(expectedRoundTripMs());
    }
}

void Ax25Connection::logSessionClose(bool byPeer)
{
    if (m_sessionStartMs == 0)
        return; // never reached Connected; nothing worth summarising
    qCInfo(lcAx25Link).noquote()
        << QStringLiteral("link down peer=%1 endedBy=%2 %3")
               .arg(m_remote.toString(),
                    byPeer ? QStringLiteral("peer") : QStringLiteral("local"),
                    sessionSummary());
}

void Ax25Connection::startT2()
{
    m_t2->start(m_t2Ms);
}

void Ax25Connection::stopT2()
{
    m_t2->stop();
}

void Ax25Connection::onT2Timeout()
{
    // The peer's burst has gone idle; send the single coalesced acknowledgement.
    if (m_state == State::Connected && m_ackPending) {
        ++m_stats.t2Acks;
        sendAck(/*pollFinal=*/false);
    }
    // The burst is over and we owe nothing: start watching for a silent peer.
    armT3IfIdle();
}

void Ax25Connection::sendAck(bool pollFinal)
{
    stopT2();
    m_ackPending = false;
    sendSupervisory(FrameType::RR, pollFinal, /*command=*/false);
}

QByteArray Ax25Connection::transmit(Frame frame)
{
    if (frame.type == FrameType::I)
        ++m_stats.iSent;
    else if (frame.type == FrameType::REJ)
        ++m_stats.rejSent;
    frame.via = m_via; // attach the digipeater path (H=0, not yet repeated)
    emit activity(QStringLiteral("TX %1 %2>%3%4%5")
        .arg(ax25::frameTypeName(frame.type),
             frame.src.toString(),
             frame.dest.toString(),
             frame.type == FrameType::I
                 ? QStringLiteral(" NS=%1 NR=%2").arg(frame.ns).arg(frame.nr)
                 : (frame.type == FrameType::RR || frame.type == FrameType::RNR
                    || frame.type == FrameType::REJ)
                       ? QStringLiteral(" NR=%1").arg(frame.nr)
                       : QString(),
             frame.pollFinal ? QStringLiteral(" P/F") : QString()));
    const QByteArray raw = frame.encode();
    emit sendFrame(raw);
    return raw;
}

void Ax25Connection::sendUFrame(FrameType type, bool pollFinal, bool command)
{
    transmit(Frame::makeU(m_remote, m_local, type, pollFinal, command));
}

void Ax25Connection::sendSupervisory(FrameType type, bool pollFinal, bool command)
{
    transmit(Frame::makeS(m_remote, m_local, type, m_vr, pollFinal, command));
}

void Ax25Connection::enterConnected(const Address& peer)
{
    m_remote = peer;
    m_state = State::Connected;
    m_vs = m_vr = m_va = 0;
    m_retryCount = 0;
    m_rejRecoveries = 0;
    m_peerBusy = false;
    m_ackPending = false;
    m_rejectSent = false;
    m_stats = Stats{}; // fresh telemetry for this session
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    for (qint64& sentMs : m_iFrameSentMs)
        sentMs = -1;
    for (bool& resent : m_iFrameResent)
        resent = false;
    m_sessionStartMs = m_clock.elapsed();
    stopT1();
    stopT2();
    logSessionOpen();
    emit activity(QStringLiteral("Connected to %1").arg(peer.toString()));
    emit connected(peer);
    // Nothing is in flight yet, so start watching for a peer that goes quiet.
    armT3IfIdle();
}

void Ax25Connection::enterDisconnected(bool byPeer)
{
    const Address peer = m_remote;
    logSessionClose(byPeer);
    stopT1();
    stopT2();
    stopT3();
    m_state = State::Disconnected;
    m_sessionStartMs = 0;
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    for (qint64& sentMs : m_iFrameSentMs)
        sentMs = -1;
    for (bool& resent : m_iFrameResent)
        resent = false;
    m_vs = m_vr = m_va = 0;
    m_retryCount = 0;
    m_rejRecoveries = 0;
    m_peerBusy = false;
    m_ackPending = false;
    m_rejectSent = false;
    m_remote = Address{};
    m_via.clear();
    m_local = m_primary; // back to answering on either address when idle
    emit activity(QStringLiteral("Disconnected from %1 (%2)")
        .arg(peer.toString(), byPeer ? QStringLiteral("by peer") : QStringLiteral("local")));
    emit disconnected(peer, byPeer);
}

void Ax25Connection::connectTo(const Address& peer, const QVector<Address>& via)
{
    if (m_state != State::Disconnected || !peer.isValid())
        return;
    m_via.clear();
    for (Address hop : via) { // freshly-keyed: none repeated yet
        hop.hasBeenRepeated = false;
        m_via.append(hop);
    }
    m_local = m_primary; // outbound calls always go out under our primary address
    m_remote = peer;
    m_state = State::Connecting;
    m_vs = m_vr = m_va = 0;
    m_retryCount = 0;
    m_rejRecoveries = 0;
    m_peerBusy = false;
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    emit activity(QStringLiteral("Connecting to %1").arg(peer.toString()));
    sendUFrame(FrameType::SABM, /*pollFinal=*/true, /*command=*/true);
    startT1();
}

void Ax25Connection::onFrameReceived(const Frame& frame)
{
    // Only react to frames addressed to us. While idle we answer on either the
    // primary or the (optional) vanity alias; latch onto whichever the caller
    // dialed so every response in the session uses that address. While in a
    // session, only the dialed address matches.
    if (m_state == State::Disconnected) {
        if (frame.dest == m_primary)
            m_local = m_primary;
        else if (m_alias.isValid() && frame.dest == m_alias)
            m_local = m_alias;
        else
            return;
    } else if (frame.dest != m_local) {
        return;
    }

    emit activity(QStringLiteral("RX %1 %2>%3%4")
        .arg(ax25::frameTypeName(frame.type),
             frame.src.toString(),
             frame.dest.toString(),
             frame.type == FrameType::I
                 ? QStringLiteral(" NS=%1 NR=%2").arg(frame.ns).arg(frame.nr)
                 : QString()));

    // A *final* (F=1 on a response) answers a poll we sent, which is positive
    // proof the peer is alive and heard us — AX.25 2.2 resets the retry budget
    // on it. Note what is deliberately NOT here: the old code reset the retry
    // counter inside ackUpTo() on ANY acknowledgement, so a peer repeating a
    // stale N(R) could hold us below N2 indefinitely and we would retransmit
    // forever. Liveness must be proven, not assumed. The REJ path is bounded
    // separately by m_rejRecoveries, so this cannot reopen that loop.
    // ...and only from our actual peer. On a shared channel a third party can
    // put an F=1 response in front of us — another station calls the mailbox's
    // listen address mid-session, we answer DM, and its reply arrives addressed
    // to our callsign. Clearing the budget on that is the same defect as
    // clearing it on a stale RR, just sourced from someone else's link.
    // (AX.25 2.2 6.7.1.3 scopes this to the response to our OWN poll; filtering
    // by peer gets most of the way there without tracking poll state.)
    // m_remote is set before the SABM in connectTo(), so a UA arriving in the
    // Connecting state still qualifies.
    if (m_state != State::Disconnected && !frame.command && frame.pollFinal
        && frame.src == m_remote)
        m_retryCount = 0;

    // Lost-UA recovery. We sent a SABM and are still awaiting its UA, but the
    // peer is already exchanging connected-mode frames with us (I / RR / RNR /
    // REJ) — proof it accepted our connect and our UA was simply lost on the
    // air. Adopt the link now and let the frame be handled normally below,
    // instead of stalling in SABM retransmits. Each duplicate SABM resets the
    // peer's link state (and its prompt), so on a marginal half-duplex path this
    // is the difference between a working session and a connect that goes live
    // but never passes data. (UA and DM are handled explicitly in the switch.)
    //
    // Invariant — the fall-through into the switch below is load-bearing:
    //   * enterConnected() resets V(R)=V(S)=V(A)=0, so a peer's first
    //     post-connect I-frame at N(S)=0 lines up with the freshly-reset V(R)
    //     and the normal I-handler accepts it. With MAXFRAME=1 (today's only
    //     config) this is always the case.
    //   * The normal RR/RNR/REJ handlers call ackUpTo(frame.nr); with V(A)=
    //     V(S)=0 every legal N(R) is in-range and the ack walks zero slots.
    // If enterConnected()'s reset block is ever changed to leave V(R) non-zero
    // (e.g. a future MAXFRAME>1 path that pre-allocates send slots), this
    // adoption must re-sync V(R) to frame.ns before the fall-through — or the
    // first I-frame will be silently dropped as out-of-sequence.
    if (m_state == State::Connecting && frame.src == m_remote
        && (frame.type == FrameType::I || frame.type == FrameType::RR
            || frame.type == FrameType::RNR || frame.type == FrameType::REJ)) {
        emit activity(QStringLiteral("Adopting link to %1 (UA lost; peer already connected)")
            .arg(m_remote.toString()));
        stopT1();
        enterConnected(m_remote);
    }

    switch (frame.type) {
    case FrameType::SABM: {
        // One caller at a time: if busy with a different peer, refuse politely.
        if (m_state == State::Connected && m_remote != frame.src) {
            transmit(Frame::makeU(frame.src, m_local, FrameType::DM,
                                  frame.pollFinal, /*command=*/false));
            emit activity(QStringLiteral("Refused %1 (busy with %2)")
                .arg(frame.src.toString(), m_remote.toString()));
            return;
        }
        // Accept (new connect or reconnect). UA first, then announce.
        m_remote = frame.src;
        transmit(Frame::makeU(m_remote, m_local, FrameType::UA,
                              frame.pollFinal, /*command=*/false));
        enterConnected(frame.src);
        break;
    }
    case FrameType::SABME: {
        // A v2.2 peer asking for the mod-128 sequence space. We are mod-8 only,
        // so refuse promptly rather than staying silent: DM is what tells the
        // caller to fall back to SABM immediately instead of burning its whole
        // retry budget on an unanswered SABME. Refusing this one frame type is
        // what makes v2.2 stations connect at all.
        transmit(Frame::makeU(frame.src, m_local, FrameType::DM,
                              frame.pollFinal, /*command=*/false));
        emit activity(QStringLiteral("Refused SABME from %1 (mod-128 unsupported; "
                                     "peer should retry with SABM)")
            .arg(frame.src.toString()));
        qCInfo(lcAx25Link).noquote()
            << QStringLiteral("link sabme-refused peer=%1 — answered DM to force v2.0 fallback")
                   .arg(frame.src.toString());
        break;
    }
    case FrameType::DISC: {
        if (m_state != State::Disconnected && frame.src == m_remote) {
            sendUFrame(FrameType::UA, frame.pollFinal, /*command=*/false);
            enterDisconnected(/*byPeer=*/true);
        } else {
            transmit(Frame::makeU(frame.src, m_local, FrameType::DM,
                                  frame.pollFinal, /*command=*/false));
        }
        break;
    }
    case FrameType::UA: {
        if (m_state == State::Connecting && frame.src == m_remote) {
            stopT1();
            enterConnected(m_remote); // our SABM was accepted
        } else if (m_state == State::Disconnecting) {
            enterDisconnected(/*byPeer=*/false);
        }
        break;
    }
    case FrameType::DM: {
        if (m_state == State::Connecting && frame.src == m_remote) {
            // The peer refused our connect request.
            emit activity(QStringLiteral("Connect refused by %1 (DM)").arg(m_remote.toString()));
            emit connectFailed(m_remote, QStringLiteral("refused (DM)"));
            enterDisconnected(/*byPeer=*/true);
        } else if (m_state != State::Disconnected) {
            enterDisconnected(/*byPeer=*/true);
        }
        break;
    }
    case FrameType::I: {
        if (m_state != State::Connected) {
            transmit(Frame::makeU(frame.src, m_local, FrameType::DM,
                                  frame.pollFinal, /*command=*/false));
            break;
        }
        ackUpTo(frame.nr);
        if (frame.ns == m_vr) {
            // In-sequence: accept and advance V(R). Clears any reject exception.
            ++m_stats.iRcvd;
            m_rejectSent = false;
            if (!frame.info.isEmpty()) {
                m_stats.infoBytesReceived += static_cast<quint64>(frame.info.size());
                emit dataReceived(frame.info);
            }
            m_vr = (m_vr + 1) % 8;
            m_ackPending = true;
            // If we have data to send, it piggybacks the ack (N(R)) for free.
            pumpOutbound();
            if (m_ackPending) {
                if (frame.pollFinal) {
                    // The peer polled us: it has stopped transmitting and is
                    // waiting for a final, so ack immediately.
                    sendAck(/*pollFinal=*/true);
                } else {
                    // Unpolled mid-burst frame: defer the ack so we don't key the
                    // radio (and go deaf) while the peer is still sending the rest
                    // of its window. T2 sends the coalesced RR once the burst ends.
                    startT2();
                }
            }
        } else if (isDuplicateIFrame(frame.ns)) {
            // A frame we have ALREADY accepted, sent again. That means our
            // acknowledgement was lost, not the data — so re-send the ack.
            //
            // This case used to fall into the reject-exception branch below and
            // be answered with silence, which is a guaranteed deadlock: the peer
            // retransmits on T1, we refuse to answer because N(S) != V(R), it
            // retransmits again, and the link dies at N2 with both ends
            // individually behaving "correctly". Observed on the air
            // 2026-07-31 — one lost RR ended every session at exactly the same
            // point, with the far end decoding all nine retransmissions
            // perfectly and deliberately saying nothing.
            //
            // Answer immediately rather than deferring via T2: with a
            // retransmission the peer has finished its burst and is listening
            // for precisely this.
            ++m_stats.iDuplicate;
            emit activity(QStringLiteral("Duplicate I NS=%1 (ack lost) — re-acking N(R)=%2")
                .arg(frame.ns).arg(m_vr));
            // The duplicate's N(R) may have acknowledged our own outstanding
            // frame up at ackUpTo(), freeing the send window — so pump before
            // acking, exactly as the in-sequence path does. Omitting this
            // stalled the link dead: with the window free and T1 stopped,
            // nothing was left to trigger the next segment, so a mailbox with
            // 516 bytes still queued sat silent for the full 126 s until T3
            // shook it loose. Observed live 2026-07-31 with iDuplicate=1.
            // An I-frame carries N(R) and acknowledges for free, so the
            // explicit RR is only needed when nothing went out.
            m_ackPending = true;
            pumpOutbound();
            if (m_ackPending)
                sendAck(/*pollFinal=*/frame.pollFinal);
        } else {
            // Out of sequence. Send REJ exactly ONCE per gap (reject exception),
            // then discard further out-of-sequence frames SILENTLY — even polled
            // ones. This is the crucial half-duplex behaviour: answering every
            // polled retransmit makes the peer retransmit immediately, and on our
            // slow radio turnaround that retransmission lands while we are still
            // keyed/switching and we miss the very frame we need (observed live
            // with SJVBBS-1: a 4-REJ phase-lock that never recovered NS=1). By
            // staying quiet we let the peer's own T1 retransmit arrive while we
            // are actually listening. We do echo the poll/final on the single REJ
            // so the peer still gets one prompt response.
            stopT2();
            m_ackPending = false;
            ++m_stats.iDropped;
            if (!m_rejectSent) {
                m_rejectSent = true;
                sendSupervisory(FrameType::REJ, /*pollFinal=*/frame.pollFinal,
                                /*command=*/false);
            }
        }
        break;
    }
    case FrameType::RR: {
        if (m_state != State::Connected)
            break;
        ++m_stats.rrRcvd;
        m_peerBusy = false;
        ackUpTo(frame.nr);
        // Answer a command poll with a final. If we're missing a frame (reject
        // exception), re-request it with a REJ rather than a plain RR, so a peer
        // that only resends on an explicit REJ knows to retransmit the gap.
        if (frame.command && frame.pollFinal)
            sendSupervisory(m_rejectSent ? FrameType::REJ : FrameType::RR,
                            /*pollFinal=*/true, /*command=*/false);
        pumpOutbound();
        break;
    }
    case FrameType::RNR: {
        if (m_state != State::Connected)
            break;
        ++m_stats.rnrRcvd;
        m_peerBusy = true;
        ackUpTo(frame.nr);
        if (frame.command && frame.pollFinal)
            sendSupervisory(m_rejectSent ? FrameType::REJ : FrameType::RR,
                            /*pollFinal=*/true, /*command=*/false);
        break;
    }
    case FrameType::REJ: {
        if (m_state != State::Connected)
            break;
        ++m_stats.rejRcvd;
        m_peerBusy = false;
        // Confirm the frames the peer DID receive (everything before N(R)).
        ackUpTo(frame.nr);
        // Bound REJ recovery. This used to be an unconditional
        // `m_retryCount = 0`, which meant a peer that REJ'd without ever
        // accepting the retransmission reset our budget on every REJ: N2 was
        // never reached and we retransmitted forever, keying the radio
        // indefinitely on a link that was never going to recover. The counter
        // is cleared by real progress (V(A) advancing) in ackUpTo().
        if (++m_rejRecoveries > m_n2) {
            emit activity(QStringLiteral("Link failure: %1 REJ recovery rounds without "
                                         "progress").arg(m_rejRecoveries - 1));
            qCWarning(lcAx25Link).noquote()
                << QStringLiteral("link rej-loop peer=%1 rounds=%2 n2=%3 vs=%4 va=%5 — "
                                  "giving up; peer never accepted a retransmission")
                       .arg(m_remote.toString())
                       .arg(m_rejRecoveries - 1)
                       .arg(m_n2)
                       .arg(m_vs)
                       .arg(m_va);
            sendUFrame(FrameType::DM, /*pollFinal=*/false, /*command=*/false);
            enterDisconnected(/*byPeer=*/true);
            break;
        }
        // Resend the still-outstanding I-frames [V(A), V(S)) from our store.
        // NOTE: we must NOT do `m_vs = frame.nr; pumpOutbound();` — the frames'
        // payload was already consumed from m_sendBuffer when first sent, so
        // pumpOutbound() would resend nothing, the link would silently desync
        // (the peer keeps REJ-ing, never receives the frame, and finally drops
        // us), and rewinding V(S) would also discard the unacked frames.
        if (outstanding() > 0)
            retransmitUnacked();   // replays [V(A), V(S)) from m_sentIFrames[]
        else
            pumpOutbound();        // nothing outstanding: push any new data
        break;
    }
    case FrameType::FRMR: {
        ++m_stats.frmrRcvd;
        // Protocol error reported by peer: re-establish by tearing down.
        if (m_state != State::Disconnected) {
            sendUFrame(FrameType::DM, /*pollFinal=*/false, /*command=*/false);
            enterDisconnected(/*byPeer=*/true);
        }
        break;
    }
    case FrameType::UI:
    case FrameType::Unknown:
        break; // UI handled elsewhere (beacons / monitor); ignore here.
    }

    // Safety net: never sit idle with data still queued. Every branch above
    // that frees the send window is meant to pump, but one that forgets stalls
    // the link outright — T1 is stopped because nothing is outstanding, and
    // nothing is left to start the next segment until T3 fires two minutes
    // later. That is a silent, total stop with the operator watching a half-
    // delivered menu, and it is worth making structural rather than trusting
    // every present and future branch to remember. Bounded by the send window,
    // so it can never key more than the one frame that branch owed anyway.
    if (m_state == State::Connected && !m_sendBuffer.isEmpty() && outstanding() < m_window)
        pumpOutbound();

    // Single choke point for idle detection: every state change reachable from
    // the air arrives through this switch, so arming here catches them all
    // (including the transitions that end the session, where it stops T3).
    armT3IfIdle();
}

void Ax25Connection::ackUpTo(int nr)
{
    nr &= 7;
    // A valid N(R) acknowledges somewhere in [V(A), V(S)]. Reject an N(R) that
    // claims frames we never sent — applying it would walk V(A) past V(S) around
    // the mod-8 ring and corrupt the send window (observed: a peer REJ/RR with a
    // stale N(R) inflated `outstanding()` and triggered bogus retransmits). AX.25
    // treats this as a link error; we conservatively ignore it.
    const int toAck = (nr - m_va + 8) % 8;
    if (toAck > outstanding()) {
        ++m_stats.invalidNr;
        emit activity(QStringLiteral("Ignoring invalid N(R)=%1 (V(A)=%2 V(S)=%3)")
            .arg(nr).arg(m_va).arg(m_vs));
        return;
    }
    // Free acknowledged I-frame slots in the range [V(A), nr).
    while (m_va != nr) {
        recordRttSample(m_va);
        m_iFrameValid[m_va] = false;
        m_iFrameSentMs[m_va] = -1;
        m_iFrameResent[m_va] = false;
        m_sentIFrames[m_va].clear();
        m_va = (m_va + 1) % 8;
    }
    // The window actually moved: the peer received something new. That is the
    // one unambiguous proof of progress, and the only thing that clears the
    // REJ-recovery counter.
    if (toAck > 0)
        noteLinkProgress();
    if (outstanding() == 0) {
        stopT1();
    } else if (toAck > 0) {
        // Only a real acknowledgement re-arms T1. Restarting it on a frame that
        // acknowledged nothing let a peer hold the timer open indefinitely: with
        // frames still outstanding, every arriving RR — including a stale one,
        // or the peer's own retransmissions — pushed the deadline out, so a lost
        // frame could go undetected for far longer than T1. Observed live:
        // t1Timeouts=0 on a link whose measured round trip was 20.4 s against a
        // T1 of 12.6 s, which made the retry statistics unusable for tuning.
        // With no progress we leave the running timer exactly where it is; it
        // was started when the outstanding frame was sent.
        startT1();
    }
    armT3IfIdle();
}

void Ax25Connection::sendData(const QByteArray& data)
{
    if (m_state != State::Connected || data.isEmpty())
        return;
    m_sendBuffer.append(data);
    pumpOutbound();
}

void Ax25Connection::pumpOutbound()
{
    if (m_state != State::Connected || m_peerBusy)
        return;
    while (!m_sendBuffer.isEmpty() && outstanding() < m_window) {
        const QByteArray segment = m_sendBuffer.left(m_paclen);
        m_sendBuffer.remove(0, segment.size());
        const int ns = m_vs;
        // Poll on the frame that fills the window or empties our buffer (for the
        // default window=1, that's every frame). P=1 obliges the half-duplex peer
        // to acknowledge immediately rather than deferring its ack — without it,
        // a peer that batches acks leaves our frame unacknowledged until T1 and we
        // retransmit needlessly (the observed multi-second command stalls).
        const bool poll = m_sendBuffer.isEmpty() || (outstanding() + 1) >= m_window;
        Frame iFrame = Frame::makeI(m_remote, m_local, ns, m_vr,
                                    /*pollFinal=*/poll, segment);
        m_iFrameValid[ns] = true;
        // First transmission of this slot — the only stamp that yields a usable
        // round-trip sample (see recordRttSample / Karn's algorithm).
        m_iFrameSentMs[ns] = m_clock.elapsed();
        m_iFrameResent[ns] = false;
        m_stats.infoBytesSent += static_cast<quint64>(segment.size());
        m_vs = (m_vs + 1) % 8;
        // Store the exact bytes sent (digipeater path included) for retransmit.
        m_sentIFrames[ns] = transmit(iFrame);
        // The I-frame carries N(R) = V(R), so it acknowledges everything we have
        // received — no separate RR needed, and cancel any deferred ack.
        m_ackPending = false;
        stopT2();
        startT1();
    }
}

void Ax25Connection::retransmitUnacked()
{
    // Resend every unacknowledged I-frame, polling on the last to solicit an ack.
    int seq = m_va;
    int count = outstanding();
    int sent = 0;
    while (count-- > 0) {
        if (m_iFrameValid[seq]) {
            QByteArray raw = m_sentIFrames[seq];
            // Update N(R) and set poll on the final retransmitted frame.
            auto decoded = Frame::decode(raw);
            if (decoded) {
                decoded->nr = m_vr;
                decoded->pollFinal = (count == 0);
                raw = decoded->encode();
                m_sentIFrames[seq] = raw;
            }
            emit sendFrame(raw);
            ++sent;
            ++m_stats.iResent;
            // Poison this slot's RTT sample: once two copies are on the air we
            // cannot tell which one an ack answers (Karn's algorithm).
            m_iFrameResent[seq] = true;
        }
        seq = (seq + 1) % 8;
    }
    if (sent == 0) {
        // Nothing to resend; poll the peer to checkpoint.
        sendSupervisory(FrameType::RR, /*pollFinal=*/true, /*command=*/true);
    }
    emit activity(QStringLiteral("T1 retransmit (%1 frame(s), try %2/%3)")
        .arg(sent).arg(m_retryCount).arg(m_n2));
    startT1();
}

void Ax25Connection::onT1Timeout()
{
    if (m_state == State::Disconnected)
        return;
    ++m_stats.t1Timeouts;

    // The diagnostic that tells a mis-sized timer apart from a bad channel. If
    // the round trips we have actually measured are longer than T1, no channel
    // improvement can help — the timer fires before the ack can physically
    // arrive, which is precisely how HF connected mode was broken.
    const qint64 avgRtt = m_stats.averageRttMs();
    const bool t1TooShort = (m_stats.rttSamples > 0 && avgRtt >= m_t1Ms)
        || m_t1Ms < expectedRoundTripMs();
    qCInfo(lcAx25Link).noquote()
        << QStringLiteral("link t1-expired peer=%1 state=%2 t1Ms=%3 retry=%4/%5 "
                          "outstanding=%6 avgRttMs=%7 samples=%8 modelRttMs=%9%10")
               .arg(m_remote.toString())
               .arg(m_state == State::Connecting ? QStringLiteral("connecting")
                    : m_state == State::Disconnecting ? QStringLiteral("disconnecting")
                                                      : QStringLiteral("connected"))
               .arg(m_t1Ms)
               .arg(m_retryCount)
               .arg(m_n2)
               .arg(outstanding())
               .arg(avgRtt)
               .arg(m_stats.rttSamples)
               .arg(expectedRoundTripMs())
               .arg(t1TooShort ? QStringLiteral(" T1_TOO_SHORT") : QString());

    if (m_retryCount >= m_n2) {
        if (m_state == State::Connecting) {
            emit activity(QStringLiteral("Connect failed: no response from %1 after %2 retries")
                .arg(m_remote.toString()).arg(m_n2));
            emit connectFailed(m_remote, QStringLiteral("no response"));
            enterDisconnected(/*byPeer=*/true);
            return;
        }
        emit activity(QStringLiteral("Link failure: no response after %1 retries").arg(m_n2));
        if (m_state == State::Connected || m_state == State::Disconnecting) {
            sendUFrame(FrameType::DM, /*pollFinal=*/false, /*command=*/false);
            enterDisconnected(/*byPeer=*/true);
        }
        return;
    }
    ++m_retryCount;

    if (m_state == State::Connecting) {
        emit activity(QStringLiteral("SABM retry %1/%2").arg(m_retryCount).arg(m_n2));
        sendUFrame(FrameType::SABM, /*pollFinal=*/true, /*command=*/true);
        startT1();
        return;
    }

    if (m_state == State::Disconnecting) {
        sendUFrame(FrameType::DISC, /*pollFinal=*/true, /*command=*/true);
        startT1();
        return;
    }
    retransmitUnacked();
}

void Ax25Connection::disconnect()
{
    if (m_state == State::Disconnected)
        return;

    // A second disconnect request while we are ALREADY tearing down means the
    // operator wants out now — not a politer retry. The old code restarted the
    // teardown unconditionally, including `m_retryCount = 0`, so every press of
    // BYE handed the DISC retransmissions a fresh N2 budget: the command that
    // was supposed to stop the transmissions was the thing keeping them going.
    // Drop the link silently instead (enterDisconnected() sends nothing).
    if (m_state == State::Disconnecting) {
        emit activity(QStringLiteral(
            "Disconnect forced — dropping the link without waiting for a UA"));
        enterDisconnected(/*byPeer=*/false);
        return;
    }

    m_state = State::Disconnecting;
    m_retryCount = 0;
    m_sendBuffer.clear();
    sendUFrame(FrameType::DISC, /*pollFinal=*/true, /*command=*/true);
    startT1();
}

void Ax25Connection::reset()
{
    if (m_state != State::Disconnected) {
        enterDisconnected(/*byPeer=*/false);
    } else {
        stopT1();
        stopT2();
        stopT3();
    }
}

} // namespace AetherSDR
