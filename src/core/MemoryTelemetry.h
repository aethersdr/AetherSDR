#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QtGlobal>

#include <deque>

namespace AetherSDR {

// Cross-platform, current-process memory counters. The resident metric follows
// the number operators see in the native task monitor: Working Set on Windows,
// physical footprint on macOS, and VmRSS on Linux. The other fields are
// best-effort because the three kernels expose different accounting models.
struct ProcessMemorySnapshot {
    bool valid{false};
    QString platform;
    QString residentMetric;
    quint64 residentBytes{0};
    quint64 peakResidentBytes{0};
    quint64 privateBytes{0};
    quint64 virtualBytes{0};
    quint64 allocatorInUseBytes{0};
    quint64 allocatorReservedBytes{0};
    qint64 threadCount{-1};
    qint64 handleCount{-1};

    static ProcessMemorySnapshot capture();
    QJsonObject toJson() const;
};

// A compact, bounded time series used by the automation bridge's `memory`
// profiler. It accepts the bridge snapshot schema instead of depending on GUI
// classes, which keeps the trend math deterministic and independently testable.
class MemoryTelemetrySeries final {
public:
    explicit MemoryTelemetrySeries(int maxSamples = 10000);

    void clear();
    void setMaxSamples(int maxSamples);
    int maxSamples() const { return m_maxSamples; }
    int sampleCount() const { return static_cast<int>(m_points.size()); }
    bool isEmpty() const { return m_points.empty(); }

    void addSnapshot(qint64 elapsedMs, const QJsonObject& snapshot);
    QJsonObject report(bool includeSamples = false) const;
    quint64 estimatedStorageBytes() const;

private:
    struct Point {
        qint64 elapsedMs{0};
        QMap<QString, double> byteMetrics;
        QMap<QString, double> countMetrics;
    };

    static QJsonObject trend(const std::deque<Point>& points,
                             const QString& metric,
                             bool bytes);
    static QJsonObject pointToJson(const Point& point);
    static QMap<QString, qint64> jsonCountMap(const QJsonObject& object);
    static QJsonObject classGrowth(
        const QMap<QString, QMap<QString, qint64>>& first,
        const QMap<QString, QMap<QString, qint64>>& last);

    int m_maxSamples{10000};
    std::deque<Point> m_points;
    QMap<QString, QMap<QString, qint64>> m_firstClasses;
    QMap<QString, QMap<QString, qint64>> m_lastClasses;
};

} // namespace AetherSDR
