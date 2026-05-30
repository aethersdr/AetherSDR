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

void Ax25Connection::transmit(const Frame& frame)
{
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
    emit sendFrame(frame.encode());
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
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    stopT1();
    emit activity(QStringLiteral("Connected to %1").arg(peer.toString()));
    emit connected(peer);
}

void Ax25Connection::enterDisconnected(bool byPeer)
{
    const Address peer = m_remote;
    stopT1();
    m_state = State::Disconnected;
    m_sendBuffer.clear();
    for (bool& valid : m_iFrameValid)
        valid = false;
    m_vs = m_vr = m_va = 0;
    m_retryCount = 0;
    m_peerBusy = false;
    m_remote = Address{};
    m_local = m_primary; // back to answering on either address when idle
    emit activity(QStringLiteral("Disconnected from %1 (%2)")
        .arg(peer.toString(), byPeer ? QStringLiteral("by peer") : QStringLiteral("local")));
    emit disconnected(peer, byPeer);
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
        if (m_state == State::Disconnecting)
            enterDisconnected(/*byPeer=*/false);
        break;
    }
    case FrameType::DM: {
        if (m_state != State::Disconnected)
            enterDisconnected(/*byPeer=*/true);
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
            // In-sequence: accept and advance V(R).
            if (!frame.info.isEmpty())
                emit dataReceived(frame.info);
            m_vr = (m_vr + 1) % 8;
            pumpOutbound();
            // Acknowledge. A command with the poll bit demands a final response.
            sendSupervisory(FrameType::RR, /*pollFinal=*/frame.pollFinal,
                            /*command=*/false);
        } else {
            // Out of sequence: ask for retransmission from V(R).
            sendSupervisory(FrameType::REJ, /*pollFinal=*/frame.pollFinal,
                            /*command=*/false);
        }
        break;
    }
    case FrameType::RR: {
        if (m_state != State::Connected)
            break;
        m_peerBusy = false;
        ackUpTo(frame.nr);
        // A command poll requires us to respond with a final.
        if (frame.command && frame.pollFinal)
            sendSupervisory(FrameType::RR, /*pollFinal=*/true, /*command=*/false);
        pumpOutbound();
        break;
    }
    case FrameType::RNR: {
        if (m_state != State::Connected)
            break;
        m_peerBusy = true;
        ackUpTo(frame.nr);
        if (frame.command && frame.pollFinal)
            sendSupervisory(FrameType::RR, /*pollFinal=*/true, /*command=*/false);
        break;
    }
    case FrameType::REJ: {
        if (m_state != State::Connected)
            break;
        m_peerBusy = false;
        ackUpTo(frame.nr);
        // Retransmit everything from the rejected sequence number forward.
        m_vs = frame.nr;
        m_retryCount = 0;
        pumpOutbound();
        break;
    }
    case FrameType::FRMR: {
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
        Frame iFrame = Frame::makeI(m_remote, m_local, ns, m_vr,
                                    /*pollFinal=*/false, segment);
        m_sentIFrames[ns] = iFrame.encode();
        m_iFrameValid[ns] = true;
        m_vs = (m_vs + 1) % 8;
        transmit(iFrame);
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

    if (m_retryCount >= m_n2) {
        emit activity(QStringLiteral("Link failure: no response after %1 retries").arg(m_n2));
        if (m_state == State::Connected || m_state == State::Disconnecting) {
            sendUFrame(FrameType::DM, /*pollFinal=*/false, /*command=*/false);
            enterDisconnected(/*byPeer=*/true);
        }
        return;
    }
    ++m_retryCount;

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
    if (m_state != State::Disconnected)
        enterDisconnected(/*byPeer=*/false);
    else
        stopT1();
}

} // namespace AetherSDR
