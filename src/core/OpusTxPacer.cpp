#include "OpusTxPacer.h"

#include <QtEndian>

#include <algorithm>
#include <utility>

namespace AetherSDR {

bool OpusTxPacer::enqueue(QByteArray packet)
{
    bool droppedOldest = false;
    if (m_queue.size() >= kMaxQueuePackets) {
        m_queue.removeFirst();
        ++m_droppedPackets;
        droppedOldest = true;
    }

    m_queue.append(std::move(packet));
    m_maxQueueDepth = std::max(
        m_maxQueueDepth, static_cast<int>(m_queue.size()));
    return droppedOldest;
}

OpusTxPacer::DrainResult OpusTxPacer::takeDue(qint64 nowMs,
                                              quint8& packetCount)
{
    DrainResult result;
    if (m_queue.isEmpty()) {
        // The pacing timer free-runs from the AudioEngine constructor, so this
        // fires on every 10 ms tick with no audio queued — between overs, on a
        // mic xrun, while RN2 re-warms. No deadline was actually missed on
        // those ticks, so re-anchor to the next interval. Leaving the deadline
        // behind the wall clock books debt repayable only at one extra packet
        // per drain, which a real-time producer never affords: the pacer would
        // degrade into "flush up to kMaxPacketsPerDrain whenever packets exist"
        // for the rest of the over — the back-to-back bursts it exists to
        // smooth. A 30 ms drought was enough to trigger this permanently.
        if (m_nextSendDeadlineMs >= 0 && nowMs >= m_nextSendDeadlineMs) {
            m_nextSendDeadlineMs = nowMs + kPacketIntervalMs;
        }
        return result;
    }

    if (m_nextSendDeadlineMs < 0) {
        m_nextSendDeadlineMs = nowMs;
    }
    if (nowMs < m_nextSendDeadlineMs) {
        return result;
    }

    const qint64 overdueIntervals =
        (nowMs - m_nextSendDeadlineMs) / kPacketIntervalMs;
    const int duePackets = static_cast<int>(std::min<qint64>(
        kMaxPacketsPerDrain, overdueIntervals + 1));
    const int sendCount = std::min(
        duePackets, static_cast<int>(m_queue.size()));
    result.packets.reserve(sendCount);

    for (int i = 0; i < sendCount; ++i) {
        QByteArray packet = m_queue.takeFirst();
        stampPacketCount(packet, packetCount);
        packetCount = static_cast<quint8>((packetCount + 1) & 0x0F);
        result.packets.append(std::move(packet));
    }

    result.catchUpPackets = std::max(0, sendCount - 1);
    m_packetsSent += static_cast<quint64>(sendCount);
    m_catchUpPackets += static_cast<quint64>(result.catchUpPackets);
    m_nextSendDeadlineMs +=
        static_cast<qint64>(sendCount) * kPacketIntervalMs;

    return result;
}

void OpusTxPacer::clear()
{
    m_queue.clear();
    m_nextSendDeadlineMs = -1;
}

void OpusTxPacer::stampPacketCount(QByteArray& packet, quint8 packetCount)
{
    // The only producer builds a full 28-byte VITA-49 ExtDataWithStream
    // header (AudioEngine::onTxAudioReady). Anything shorter is not a packet
    // this pacer knows how to stamp, so leave it untouched rather than
    // rewriting whatever happens to occupy the first word.
    if (packet.size() < kVitaHeaderBytes) {
        return;
    }

    const auto* source =
        reinterpret_cast<const uchar*>(packet.constData());
    quint32 header = qFromBigEndian<quint32>(source);
    header &= ~(0x0Fu << 16);
    header |= (static_cast<quint32>(packetCount & 0x0F) << 16);
    qToBigEndian<quint32>(
        header, reinterpret_cast<uchar*>(packet.data()));
}

} // namespace AetherSDR
