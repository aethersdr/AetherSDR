#pragma once

#include "core/ReceiveData.h"
#include "core/backends/MeterDef.h"
#include <QHash>
#include <QMutex>
#include <deque>
#include <functional>
#include <optional>
#include <variant>

namespace AetherSDR {

// Shared only by Flex's decoder and owner thread. The notifier posts a single
// owner-thread wake; it must not deliver signals or call back into this object.
class FlexReceiveData final {
public:
    using Display = std::variant<ReceiveSpectrumFrame, ReceiveWaterfallRow>;
    using Event = std::variant<ReceiveSpectrumFrame, ReceiveWaterfallRow,
                               ReceiveMeterBatch, ReceiveTelemetry>;
    using Wake = std::function<void(quint64)>;
    static constexpr qsizetype kMaxBins = 65535;
    static constexpr qsizetype kMaxStreams = 64;
    static constexpr qsizetype kMaxDefinitions = 4096;
    static constexpr qsizetype kMaxDisplayBytes = 8 * 1024 * 1024;
    static constexpr qsizetype kMaxDisplayFrames = 256;
    static constexpr qsizetype kRowsPerStream = 32;
    static constexpr qsizetype kMaxMeterBytes = 1024 * 1024;
    static constexpr qsizetype kMaxMeterBatches = 128;
    static constexpr qsizetype kDrainEvents = 16;
    static constexpr qint64 kMaxAgeNs = 500'000'000;

    explicit FlexReceiveData(Wake wake);
    ~FlexReceiveData();
    void begin();
    void retire();
    void close();
    void clearStreams();
    ReceiveDataLease sessionLease() const;
    void registerStream(quint32 id, bool waterfall);
    void unregisterStream(quint32 id, bool waterfall);
    ReceiveDataLease streamLease(quint32 id, bool waterfall) const;
    void defineMeter(const MeterDef& definition);
    void removeMeter(int id);
    void spectrum(ReceiveSpectrumFrame frame);
    void waterfall(ReceiveWaterfallRow row);
    void meters(const QVector<quint16>& ids, const QVector<qint16>& values,
                ReceiveReceipt receipt, ReceiveDataLease session = {});
    void telemetry(ReceiveTelemetry snapshot);
    bool beginDrain();
    void endDrain();
    QVector<Event> take(quint64 generation, qint64 nowNs = receiveMonotonicNs());
    ReceiveDeliveryStats stats() const;
    static float convertMeter(const QString& unit, qint16 raw);

private:
    struct Meter {
        MeterDef definition;
        std::shared_ptr<ReceiveDataEpoch> epoch;
    };
    void retireLocked();
    void wakeLocked();
    void accountLocked();
    void dropDisplayLocked(std::deque<Display>::iterator it);
    void dropMeterLocked();
    void displayLocked(Display value);
    static qsizetype bytes(const Display& value);
    static qsizetype bytes(const ReceiveMeterBatch& value);
    static quint32 streamId(const Display& value);
    mutable QMutex m_mutex;
    Wake m_wake;
    quint64 m_generation = 0;
    std::shared_ptr<ReceiveDataEpoch> m_session;
    QHash<quint32, std::shared_ptr<ReceiveDataEpoch>> m_pans;
    QHash<quint32, std::shared_ptr<ReceiveDataEpoch>> m_waterfalls;
    QHash<int, Meter> m_definitions;
    std::deque<Display> m_display;
    std::deque<ReceiveMeterBatch> m_meters;
    std::optional<ReceiveTelemetry> m_telemetry;
    ReceiveDeliveryStats m_stats;
    qsizetype m_displayBytes = 0;
    qsizetype m_meterBytes = 0;
    bool m_scheduled = false;
    bool m_draining = false;
    bool m_meterTurn = false;
};

} // namespace AetherSDR
