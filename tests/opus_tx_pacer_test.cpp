#include "core/OpusTxPacer.h"

#include <QtEndian>

#include <cstdio>
#include <limits>

using AetherSDR::OpusTxPacer;

namespace {

QByteArray makePacket(quint8 marker)
{
    constexpr quint32 kHeaderWithoutCount = 0x38D00010u;
    QByteArray packet(8, '\0');
    qToBigEndian<quint32>(
        kHeaderWithoutCount,
        reinterpret_cast<uchar*>(packet.data()));
    packet[4] = static_cast<char>(marker);
    return packet;
}

int packetCount(const QByteArray& packet)
{
    const quint32 header = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(packet.constData()));
    return static_cast<int>((header >> 16) & 0x0F);
}

bool packetHeaderExceptCountIsPreserved(const QByteArray& packet)
{
    constexpr quint32 kCountMask = 0x000F0000u;
    constexpr quint32 kHeaderWithoutCount = 0x38D00010u;
    const quint32 header = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(packet.constData()));
    return (header & ~kCountMask) == kHeaderWithoutCount;
}

bool testLateTimerCatchesUp()
{
    OpusTxPacer pacer;
    for (int i = 0; i < 6; ++i) {
        pacer.enqueue(makePacket(static_cast<quint8>(i)));
    }

    quint8 count = 0;
    const OpusTxPacer::DrainResult first = pacer.takeDue(100, count);
    const OpusTxPacer::DrainResult late = pacer.takeDue(130, count);

    if (first.packets.size() != 1 || late.packets.size() != 3
        || late.catchUpPackets != 2 || pacer.queueDepth() != 2) {
        std::printf(
            "late catch-up mismatch: first=%lld late=%lld catchUp=%d depth=%d\n",
            static_cast<long long>(first.packets.size()),
            static_cast<long long>(late.packets.size()),
            late.catchUpPackets,
            pacer.queueDepth());
        return false;
    }
    return true;
}

bool testCatchUpIsBounded()
{
    OpusTxPacer pacer;
    for (int i = 0; i < 10; ++i) {
        pacer.enqueue(makePacket(static_cast<quint8>(i)));
    }

    quint8 count = 0;
    pacer.takeDue(0, count);
    const OpusTxPacer::DrainResult veryLate =
        pacer.takeDue(std::numeric_limits<qint64>::max(), count);
    if (veryLate.packets.size() != OpusTxPacer::kMaxPacketsPerDrain) {
        std::printf("unbounded catch-up: sent=%lld\n",
                    static_cast<long long>(veryLate.packets.size()));
        return false;
    }
    return true;
}

bool testOverflowKeepsWireCountsContiguous()
{
    OpusTxPacer pacer;
    bool dropped = false;
    for (int i = 0; i <= OpusTxPacer::kMaxQueuePackets; ++i) {
        dropped = pacer.enqueue(makePacket(static_cast<quint8>(i))) || dropped;
    }
    if (!dropped || pacer.droppedPackets() != 1
        || pacer.queueDepth() != OpusTxPacer::kMaxQueuePackets) {
        std::printf("overflow accounting mismatch\n");
        return false;
    }

    quint8 count = 14;
    QVector<QByteArray> sent;
    qint64 nowMs = 0;
    while (pacer.queueDepth() > 0) {
        const OpusTxPacer::DrainResult result = pacer.takeDue(nowMs, count);
        sent.append(result.packets);
        nowMs += 30;
    }

    if (sent.size() != OpusTxPacer::kMaxQueuePackets
        || static_cast<quint8>(sent.first()[4]) != 1) {
        std::printf("overflow did not discard exactly the oldest packet\n");
        return false;
    }
    for (int i = 0; i < sent.size(); ++i) {
        const int expected = (14 + i) & 0x0F;
        if (packetCount(sent[i]) != expected) {
            std::printf("packet count gap at %d: got=%d expected=%d\n",
                        i, packetCount(sent[i]), expected);
            return false;
        }
        if (!packetHeaderExceptCountIsPreserved(sent[i])) {
            std::printf("packet header changed outside count at %d\n", i);
            return false;
        }
    }
    return true;
}

bool testIdleQueueResumesWithoutDelay()
{
    OpusTxPacer pacer;
    quint8 count = 0;
    pacer.enqueue(makePacket(1));
    if (pacer.takeDue(10, count).packets.size() != 1) {
        return false;
    }

    pacer.enqueue(makePacket(2));
    if (pacer.takeDue(5000, count).packets.size() != 1) {
        std::printf("idle queue did not resume immediately\n");
        return false;
    }
    return true;
}

bool testLateProducerUsesExistingSchedule()
{
    OpusTxPacer pacer;
    quint8 count = 0;
    pacer.enqueue(makePacket(0));
    if (pacer.takeDue(100, count).packets.size() != 1
        || pacer.queueDepth() != 0) {
        return false;
    }

    for (int i = 1; i <= 4; ++i) {
        pacer.enqueue(makePacket(static_cast<quint8>(i)));
    }
    const OpusTxPacer::DrainResult recovered = pacer.takeDue(130, count);
    if (recovered.packets.size() != 3
        || recovered.catchUpPackets != 2
        || pacer.queueDepth() != 1) {
        std::printf(
            "drained queue lost its schedule: sent=%lld catchUp=%d depth=%d\n",
            static_cast<long long>(recovered.packets.size()),
            recovered.catchUpPackets,
            pacer.queueDepth());
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!testLateTimerCatchesUp()
        || !testCatchUpIsBounded()
        || !testOverflowKeepsWireCountsContiguous()
        || !testIdleQueueResumesWithoutDelay()
        || !testLateProducerUsesExistingSchedule()) {
        return 1;
    }
    std::printf("opus_tx_pacer_test passed\n");
    return 0;
}
