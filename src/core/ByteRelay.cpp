#include "ByteRelay.h"

#include <QTimer>

#include <algorithm>

namespace AetherSDR {

ByteRelay::ByteRelay(ByteRelayEndpoint* a, ByteRelayEndpoint* b,
                     const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_ep{a, b}
    , m_limits(limits)
{
    m_progressTimer = new QTimer(this);
    m_progressTimer->setSingleShot(true);
    m_progressTimer->setInterval(std::max(1, m_limits.progressTimeoutMs));
    connect(m_progressTimer, &QTimer::timeout, this, [this] {
        finish(EndReason::ProgressTimeout,
               QStringLiteral("No forward progress for %1 ms with bytes pending")
                   .arg(m_limits.progressTimeoutMs),
               false);
    });

    m_drainTimer = new QTimer(this);
    m_drainTimer->setSingleShot(true);
    m_drainTimer->setInterval(std::max(1, m_limits.drainTimeoutMs));
    connect(m_drainTimer, &QTimer::timeout, this, [this] {
        finish(EndReason::DrainTimeout,
               QStringLiteral("Drain after peer close did not finish within %1 ms")
                   .arg(m_limits.drainTimeoutMs),
               false);
    });
}

ByteRelay::~ByteRelay() = default;

void ByteRelay::start()
{
    if (m_started || m_finished) {
        return;
    }
    m_started = true;
    // Either side may already hold bytes, e.g. an unsolicited greeting from
    // the upstream peer that arrived before the relay existed.
    pump(Side::B);
    pump(Side::A);
    for (const Side s : {Side::A, Side::B}) {
        if (!m_finished && !m_draining && m_inputEnded[idx(s)]) {
            beginDrain(s);
        }
    }
}

void ByteRelay::stop()
{
    finish(EndReason::Stopped, QStringLiteral("Stopped"), false);
}

qint64 ByteRelay::queuedBytes(Side src) const
{
    const qint64 unsent = std::max<qint64>(0, endpoint(other(src))->bytesToWrite());
    return m_dir[idx(src)].pending.size() + unsent;
}

ByteRelay::Stats ByteRelay::stats() const
{
    Stats s;
    s.forwarded = m_forwarded;
    if (!m_finished) {
        s.queued = {queuedBytes(Side::A), queuedBytes(Side::B)};
    }
    return s;
}

bool ByteRelay::flushPending(Side src)
{
    Direction& d = m_dir[idx(src)];
    ByteRelayEndpoint* dst = endpoint(other(src));
    while (!d.pending.isEmpty()) {
        const qint64 n = dst->write(d.pending);
        if (m_finished) {
            return false;
        }
        if (n < 0) {
            finish(EndReason::TransportError,
                   QStringLiteral("Write failed toward side %1").arg(idx(other(src))),
                   false);
            return false;
        }
        if (n == 0) {
            break;
        }
        const qint64 accepted = std::min<qint64>(n, d.pending.size());
        d.pending.remove(0, accepted);
        m_forwarded[idx(src)] += static_cast<quint64>(accepted);
        m_progressTimer->start();
    }
    return true;
}

void ByteRelay::pump(Side src)
{
    if (!m_started || m_finished || m_closingGracefully) {
        return;
    }
    if (m_draining && src != m_drainSource) {
        return;
    }
    if (!flushPending(src)) {
        return;
    }

    Direction& d = m_dir[idx(src)];
    ByteRelayEndpoint* in = endpoint(src);
    ByteRelayEndpoint* out = endpoint(other(src));
    qint64 sliceLeft = m_limits.sliceBytes;

    while (sliceLeft > 0 && d.pending.isEmpty()) {
        const qint64 room = m_limits.directionBudget - queuedBytes(src);
        const qint64 avail = in->bytesAvailable();
        if (room <= 0 || avail <= 0) {
            break;
        }
        const qint64 want = std::min({room, sliceLeft, m_limits.readChunk, avail});
        const QByteArray chunk = in->read(want);
        if (m_finished) {
            return;
        }
        if (chunk.isEmpty()) {
            break;
        }
        sliceLeft -= chunk.size();

        const qint64 n = out->write(chunk);
        if (m_finished) {
            return;
        }
        if (n < 0) {
            finish(EndReason::TransportError,
                   QStringLiteral("Write failed toward side %1").arg(idx(other(src))),
                   false);
            return;
        }
        const qint64 accepted = std::min<qint64>(n, chunk.size());
        m_forwarded[idx(src)] += static_cast<quint64>(accepted);
        if (accepted > 0) {
            m_progressTimer->start();
        }
        if (accepted < chunk.size()) {
            d.pending = chunk.mid(accepted);
        }
    }

    if (sliceLeft <= 0 && d.pending.isEmpty()
        && in->bytesAvailable() > 0
        && queuedBytes(src) < m_limits.directionBudget) {
        scheduleContinuation(src);
    }

    updateProgressTimer();
    emit statsChanged();
    if (m_draining) {
        maybeFinishDrain();
    }
}

void ByteRelay::scheduleContinuation(Side src)
{
    Direction& d = m_dir[idx(src)];
    if (d.continuationScheduled) {
        return;
    }
    d.continuationScheduled = true;
    QTimer::singleShot(0, this, [this, src] {
        m_dir[idx(src)].continuationScheduled = false;
        pump(src);
    });
}

void ByteRelay::updateProgressTimer()
{
    if (m_finished || m_draining) {
        m_progressTimer->stop();
        return;
    }
    const bool pending = queuedBytes(Side::A) > 0 || queuedBytes(Side::B) > 0;
    if (!pending) {
        m_progressTimer->stop();
    } else if (!m_progressTimer->isActive()) {
        m_progressTimer->start();
    }
}

void ByteRelay::notifyReadable(Side side)
{
    pump(side);
}

void ByteRelay::notifyBytesWritten(Side side)
{
    if (m_finished) {
        return;
    }
    if (!m_draining && queuedBytes(other(side)) + queuedBytes(side) > 0) {
        m_progressTimer->start();
    }
    pump(other(side));
    if (m_draining) {
        maybeFinishDrain();
    }
}

void ByteRelay::notifyEndOfInput(Side side)
{
    if (m_finished) {
        return;
    }
    m_inputEnded[idx(side)] = true;
    if (m_started && !m_draining) {
        beginDrain(side);
    }
}

void ByteRelay::beginDrain(Side side)
{
    // Orderly EOF: forward what this side already delivered, drop the
    // reverse direction (no half-close support), then close the pair.
    m_draining = true;
    m_drainSource = side;
    m_progressTimer->stop();
    m_drainTimer->start();
    emit drainStarted();
    if (m_finished) {
        return;
    }
    pump(side);
    if (!m_finished) {
        maybeFinishDrain();
    }
}

void ByteRelay::notifyError(Side side, const QString& message)
{
    finish(EndReason::TransportError,
           QStringLiteral("Side %1: %2").arg(idx(side)).arg(message), false);
}

void ByteRelay::notifyClosed(Side side)
{
    if (m_finished) {
        return;
    }
    if (m_closingGracefully) {
        if (side == other(m_drainSource)) {
            finish(EndReason::PeerClosed, QStringLiteral("Peer closed"), true);
        }
        return;
    }
    notifyEndOfInput(side);
}

void ByteRelay::maybeFinishDrain()
{
    if (m_finished || !m_draining || m_closingGracefully) {
        return;
    }
    const Side src = m_drainSource;
    if (endpoint(src)->bytesAvailable() > 0 || !m_dir[idx(src)].pending.isEmpty()) {
        return;
    }
    // Every byte the closing side delivered is now in the destination's
    // write path; a graceful close flushes it before disconnecting.
    m_closingGracefully = true;
    const Side dst = other(src);
    endpoint(src)->closeGracefully();
    if (m_finished) {
        return;
    }
    endpoint(dst)->closeGracefully();
    if (!m_finished && endpoint(dst)->bytesToWrite() == 0 && m_inputEnded[idx(dst)]) {
        finish(EndReason::PeerClosed, QStringLiteral("Both peers closed"), true);
    }
}

void ByteRelay::finish(EndReason reason, const QString& message, bool graceful)
{
    if (m_finished) {
        return;
    }
    // Discarded bytes are what never reached a destination's write path, plus
    // what was still unsent when the pair was torn down. On a graceful close
    // the closing side's direction was fully handed off; the reverse
    // direction is dropped and counted.
    qint64 discarded[2] = {0, 0};
    for (const Side s : {Side::A, Side::B}) {
        if (graceful && m_draining && s == m_drainSource) {
            continue;
        }
        discarded[idx(s)] = queuedBytes(s);
    }
    m_finished = true;
    m_endReason = reason;
    m_endMessage = message;
    m_progressTimer->stop();
    m_drainTimer->stop();
    m_dir[0].pending.clear();
    m_dir[1].pending.clear();
    if (!graceful) {
        m_ep[0]->abort();
        m_ep[1]->abort();
    }
    emit statsChanged();
    emit finished(reason, message, discarded[0], discarded[1]);
}

} // namespace AetherSDR
