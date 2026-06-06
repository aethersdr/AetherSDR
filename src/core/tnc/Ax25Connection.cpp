#include "core/tnc/Ax25Connection.h"

#include <QTimer>

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
    m_t1->start(m_t1Ms);
}

void Ax25Connection::stopT1()
{
    m_t1->stop();
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
    m_peerBusy = false;
    m_ackPending = false;
    m_rejectSent = false;
    m_stats = Stats{}; // fresh telemetry for this session
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    stopT1();
    stopT2();
    emit activity(QStringLiteral("Connected to %1").arg(peer.toString()));
    emit connected(peer);
}

void Ax25Connection::enterDisconnected(bool byPeer)
{
    const Address peer = m_remote;
    stopT1();
    stopT2();
    m_state = State::Disconnected;
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    m_vs = m_vr = m_va = 0;
    m_retryCount = 0;
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
            if (!frame.info.isEmpty())
                emit dataReceived(frame.info);
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
        m_retryCount = 0;
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
        m_iFrameValid[m_va] = false;
        m_sentIFrames[m_va].clear();
        m_va = (m_va + 1) % 8;
    }
    m_retryCount = 0;
    if (outstanding() == 0)
        stopT1();
    else
        startT1();
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
    }
}

} // namespace AetherSDR
