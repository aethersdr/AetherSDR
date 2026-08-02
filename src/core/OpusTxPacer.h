#pragma once

#include <QByteArray>
#include <QVector>

namespace AetherSDR {

// Keeps remote_audio_tx Opus packets on a 10 ms wall-clock schedule.
// QAudioSource and DSP processing share AudioEngine's event loop with the
// pacing timer, so a late timer event must recover missed deadlines instead of
// permanently falling behind the microphone producer.
//
// Catch-up is owed only for deadlines that passed with audio actually waiting.
// Ticks that find the queue empty re-anchor the schedule instead — see
// takeDue() — so a silent gap cannot leave the pacer permanently "behind" and
// flushing bursts.
class OpusTxPacer {
public:
    static constexpr qint64 kPacketIntervalMs = 10;
    static constexpr int kMaxQueuePackets = 20;
    static constexpr int kMaxPacketsPerDrain = 3;
    // VITA-49 ExtDataWithStream header AudioEngine prepends to every payload.
    static constexpr int kVitaHeaderBytes = 28;

    struct DrainResult {
        QVector<QByteArray> packets;
        int catchUpPackets{0};
    };

    // Returns true when the oldest queued packet had to be dropped.
    bool enqueue(QByteArray packet);
    DrainResult takeDue(qint64 nowMs, quint8& packetCount);
    void clear();

    int queueDepth() const { return static_cast<int>(m_queue.size()); }
    int maxQueueDepth() const { return m_maxQueueDepth; }
    quint64 packetsSent() const { return m_packetsSent; }
    quint64 catchUpPackets() const { return m_catchUpPackets; }
    quint64 droppedPackets() const { return m_droppedPackets; }

private:
    static void stampPacketCount(QByteArray& packet, quint8 packetCount);

    QVector<QByteArray> m_queue;
    qint64 m_nextSendDeadlineMs{-1};
    int m_maxQueueDepth{0};
    quint64 m_packetsSent{0};
    quint64 m_catchUpPackets{0};
    quint64 m_droppedPackets{0};
};

} // namespace AetherSDR
