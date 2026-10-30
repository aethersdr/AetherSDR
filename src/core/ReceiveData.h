#pragma once

#include "core/SpectrumDecodeScale.h"
#include <QDateTime>
#include <QMetaType>
#include <QVector>
#include <QString>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>

namespace AetherSDR {

inline qint64 receiveMonotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ReceiveReceipt {
    qint64 monotonicNs = 0;
    qint64 utcMs = 0;
    static ReceiveReceipt now() { return {receiveMonotonicNs(), QDateTime::currentMSecsSinceEpoch()}; }
};

// A lease owns no worker or radio. Revocation also reaches frames already
// detached from the buffer and consumers retaining a frame across callbacks.
struct ReceiveDataEpoch {
    std::atomic<bool> active{true};
    std::shared_ptr<const ReceiveDataEpoch> session;
    bool current() const
    {
        return active.load(std::memory_order_acquire) && (!session || session->current());
    }
};
using ReceiveDataLease = std::shared_ptr<const ReceiveDataEpoch>;

struct ReceiveSpectrumFrame {
    quint32 streamId = 0;
    QVector<float> binsDbm;
    SpectrumDecodeScale decodeScale;
    qint64 emittedNs = 0;
    ReceiveReceipt receipt;
    ReceiveDataLease lease;
    bool current() const { return lease && lease->current(); }
};

struct ReceiveWaterfallRow {
    quint32 streamId = 0;
    QVector<float> intensity;
    double lowMhz = 0;
    double highMhz = 0;
    quint32 timecode = 0;
    quint32 autoBlack = 0;
    qint64 emittedNs = 0;
    ReceiveReceipt receipt;
    ReceiveDataLease lease;
    bool current() const { return lease && lease->current(); }
};

struct ReceiveMeterSample {
    quint16 id = 0;
    float value = 0;
    ReceiveDataLease definition;
};
struct ReceiveMeterBatch {
    QVector<ReceiveMeterSample> samples;
    ReceiveReceipt receipt;
    ReceiveDataLease lease;
    bool current() const { return lease && lease->current(); }
};

// These categories describe received traffic, not application delivery loss.
struct ReceiveStreamStats {
    enum Category { Audio, Spectrum, Waterfall, Meter, Dax, Count };
    struct Counters {
        qint64 bytes = 0;
        int packets = 0;
        int errors = 0;
    };
};

struct ReceiveDeliveryStats {
    quint64 spectrumReplaced = 0;
    quint64 waterfallDropped = 0;
    quint64 meterBatchesDropped = 0;
    quint64 rejected = 0;
    qsizetype queuedBytes = 0;
    qsizetype peakQueuedBytes = 0;
};

struct ReceiveTelemetry {
    ReceiveDataLease lease;
    ReceiveReceipt receipt;
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
    int packets = 0;
    int errors = 0;
    int gapMs = 0;
    int gapMaxMs = 0;
    int jitterMs = 0;
    bool firstPacketSeen = false;
    QString localEndpoint;
    std::array<ReceiveStreamStats::Counters, ReceiveStreamStats::Count> categories{};
    ReceiveDeliveryStats delivery;
    bool current() const { return lease && lease->current(); }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ReceiveSpectrumFrame)
Q_DECLARE_METATYPE(AetherSDR::ReceiveWaterfallRow)
Q_DECLARE_METATYPE(AetherSDR::ReceiveMeterBatch)
Q_DECLARE_METATYPE(AetherSDR::ReceiveTelemetry)
