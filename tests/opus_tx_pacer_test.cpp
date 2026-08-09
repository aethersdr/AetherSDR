#include "core/OpusTxPacer.h"

#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <limits>

using AetherSDR::OpusTxPacer;

namespace {

constexpr quint32 kHeaderWithoutCount = 0x38D00010u;
constexpr int kMarkerOffset = OpusTxPacer::kVitaHeaderBytes;

// A packet shaped like the ones AudioEngine::onTxAudioReady() builds: a full
// 28-byte VITA-49 ExtDataWithStream header followed by payload. The marker
// byte sits in the payload so a test can follow an individual packet through
// the queue without disturbing any header field.
QByteArray makePacket(quint8 marker)
{
    QByteArray packet(OpusTxPacer::kVitaHeaderBytes + 4, '\0');
    qToBigEndian<quint32>(
        kHeaderWithoutCount,
        reinterpret_cast<uchar*>(packet.data()));
    packet[kMarkerOffset] = static_cast<char>(marker);
    return packet;
}

quint8 markerOf(const QByteArray& packet)
{
    return static_cast<quint8>(packet[kMarkerOffset]);
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
    const quint32 header = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(packet.constData()));
    return (header & ~kCountMask) == kHeaderWithoutCount;
}

// A genuinely late timer event — packets were queued and waiting across the
// missed deadlines, so the pacer legitimately owes catch-up.
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
        || markerOf(sent.first()) != 1) {
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

// A drained queue must not bank catch-up credit. Ticks 110 and 120 find the
// queue empty; nothing was owed on them, so the packets that arrive afterwards
// are paced normally rather than flushed as a burst.
bool testDrainedQueueDoesNotBankCatchUp()
{
    OpusTxPacer pacer;
    quint8 count = 0;
    pacer.enqueue(makePacket(0));
    if (pacer.takeDue(100, count).packets.size() != 1
        || pacer.queueDepth() != 0) {
        return false;
    }

    pacer.takeDue(110, count);   // free-running timer, nothing queued
    pacer.takeDue(120, count);

    for (int i = 1; i <= 4; ++i) {
        pacer.enqueue(makePacket(static_cast<quint8>(i)));
    }
    const OpusTxPacer::DrainResult resumed = pacer.takeDue(130, count);
    if (resumed.packets.size() != 1
        || resumed.catchUpPackets != 0
        || pacer.queueDepth() != 3) {
        std::printf(
            "drained queue banked catch-up: sent=%lld catchUp=%d depth=%d\n",
            static_cast<long long>(resumed.packets.size()),
            resumed.catchUpPackets,
            pacer.queueDepth());
        return false;
    }
    return true;
}

// End-to-end regression for the pacer's whole reason to exist, driven the way
// AudioEngine drives it: a free-running 10 ms timer plus a bursty QAudioSource
// producer, with a mid-transmission producer drought. Before the empty-tick
// re-anchor in takeDue(), a drought as short as 30 ms permanently converted the
// pacer into "flush kMaxPacketsPerDrain back-to-back, then idle" — the jitter
// the pacer was added to remove.
bool testPacingSurvivesProducerDrought()
{
    for (const qint64 droughtMs : {30, 100, 1000}) {
        OpusTxPacer pacer;
        quint8 count = 0;
        qint64 t = 0;

        // Audio flowing at real time — primes the schedule.
        for (int i = 0; i < 20; ++i, t += OpusTxPacer::kPacketIntervalMs) {
            pacer.enqueue(makePacket(0));
            pacer.takeDue(t, count);
        }

        // Producer stalls; the pacing timer keeps firing on an empty queue.
        for (qint64 end = t + droughtMs; t < end;
             t += OpusTxPacer::kPacketIntervalMs) {
            pacer.takeDue(t, count);
        }

        // Producer resumes, handing over four 10 ms packets every 40 ms.
        int worstTick = 0;
        for (int i = 0; i < 200; ++i, t += OpusTxPacer::kPacketIntervalMs) {
            if (i % 4 == 0) {
                for (int k = 0; k < 4; ++k) {
                    pacer.enqueue(makePacket(0));
                }
            }
            worstTick = std::max<int>(
                worstTick, pacer.takeDue(t, count).packets.size());
        }

        if (worstTick != 1) {
            std::printf(
                "pacing lost after %lld ms drought: %d packets in one tick\n",
                static_cast<long long>(droughtMs), worstTick);
            return false;
        }
        if (pacer.droppedPackets() != 0) {
            std::printf("unexpected drops after %lld ms drought: %llu\n",
                        static_cast<long long>(droughtMs),
                        static_cast<unsigned long long>(pacer.droppedPackets()));
            return false;
        }
    }
    return true;
}

// Anything shorter than the VITA header is not ours to rewrite.
bool testShortPacketIsNotStamped()
{
    OpusTxPacer pacer;
    quint8 count = 7;
    QByteArray runt(OpusTxPacer::kVitaHeaderBytes - 1, '\0');
    const QByteArray original = runt;
    pacer.enqueue(runt);

    const OpusTxPacer::DrainResult drained = pacer.takeDue(0, count);
    if (drained.packets.size() != 1 || drained.packets.first() != original) {
        std::printf("short packet was modified\n");
        return false;
    }
    // The wire counter still advances so a following full packet is contiguous.
    if (count != 8) {
        std::printf("packet count did not advance: %d\n", int(count));
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
        || !testDrainedQueueDoesNotBankCatchUp()
        || !testPacingSurvivesProducerDrought()
        || !testShortPacketIsNotStamped()) {
        return 1;
    }
    std::printf("opus_tx_pacer_test passed\n");
    return 0;
}
