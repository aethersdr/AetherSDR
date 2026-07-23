#include "MemoryTelemetry.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QSysInfo>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX  // Windows.h max macro breaks std::max/numeric_limits::max.
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MAC)
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <malloc/malloc.h>
#elif defined(Q_OS_LINUX)
#if defined(__GLIBC__)
#include <features.h>
#include <malloc.h>
#endif
#endif

namespace AetherSDR {

namespace {

double jsonNumber(const QJsonObject& object, const char* key)
{
    return object.value(QString::fromLatin1(key)).toDouble(0.0);
}

#if defined(Q_OS_LINUX)
quint64 parseProcKilobytes(const QByteArray& value)
{
    const QList<QByteArray> fields = value.trimmed().split(' ');
    return fields.isEmpty() ? 0 : fields.constFirst().toULongLong() * 1024ULL;
}

QMap<QByteArray, QByteArray> readProcKeyValues(const QString& path)
{
    QMap<QByteArray, QByteArray> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return values;
    }

    QByteArray line;
    while (!(line = file.readLine()).isEmpty()) {
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        values.insert(line.left(colon), line.mid(colon + 1).trimmed());
    }
    return values;
}
#endif

} // namespace

ProcessMemorySnapshot ProcessMemorySnapshot::capture()
{
    ProcessMemorySnapshot snapshot;
    snapshot.platform = QSysInfo::productType();

#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        snapshot.valid = true;
        snapshot.residentMetric = QStringLiteral("workingSet");
        snapshot.residentBytes = static_cast<quint64>(counters.WorkingSetSize);
        snapshot.peakResidentBytes = static_cast<quint64>(counters.PeakWorkingSetSize);
        snapshot.privateBytes = static_cast<quint64>(counters.PrivateUsage);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        snapshot.handleCount = static_cast<qint64>(handles);
    }
#elif defined(Q_OS_MAC)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        snapshot.valid = true;
        snapshot.residentMetric = QStringLiteral("physicalFootprint");
        snapshot.residentBytes = static_cast<quint64>(info.phys_footprint);
        snapshot.peakResidentBytes = static_cast<quint64>(info.resident_size_peak);
        snapshot.virtualBytes = static_cast<quint64>(info.virtual_size);
        snapshot.privateBytes = static_cast<quint64>(info.internal)
            + static_cast<quint64>(info.compressed);
    }

    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    snapshot.allocatorInUseBytes = static_cast<quint64>(statistics.size_in_use);
    snapshot.allocatorReservedBytes = static_cast<quint64>(statistics.size_allocated);
#elif defined(Q_OS_LINUX)
    const QMap<QByteArray, QByteArray> status =
        readProcKeyValues(QStringLiteral("/proc/self/status"));
    snapshot.residentBytes = parseProcKilobytes(status.value("VmRSS"));
    snapshot.peakResidentBytes = parseProcKilobytes(status.value("VmHWM"));
    snapshot.virtualBytes = parseProcKilobytes(status.value("VmSize"));
    if (status.contains("Threads")) {
        snapshot.threadCount = status.value("Threads").toLongLong();
    }  // else leave the -1 "unknown" sentinel so toJson() omits it
    snapshot.valid = snapshot.residentBytes > 0;
    snapshot.residentMetric = QStringLiteral("vmRss");

    const QMap<QByteArray, QByteArray> rollup =
        readProcKeyValues(QStringLiteral("/proc/self/smaps_rollup"));
    snapshot.privateBytes = parseProcKilobytes(rollup.value("Private_Clean"))
        + parseProcKilobytes(rollup.value("Private_Dirty"));

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 allocator = mallinfo2();
    snapshot.allocatorInUseBytes = static_cast<quint64>(allocator.uordblks)
        + static_cast<quint64>(allocator.hblkhd);
    snapshot.allocatorReservedBytes = static_cast<quint64>(allocator.arena)
        + static_cast<quint64>(allocator.hblkhd);
#endif
#endif
#else
    snapshot.residentMetric = QStringLiteral("unsupported");
#endif

    return snapshot;
}

QJsonObject ProcessMemorySnapshot::toJson() const
{
    QJsonObject object{
        {QStringLiteral("valid"), valid},
        {QStringLiteral("platform"), platform},
        {QStringLiteral("residentMetric"), residentMetric},
        {QStringLiteral("residentBytes"), static_cast<double>(residentBytes)},
        {QStringLiteral("peakResidentBytes"), static_cast<double>(peakResidentBytes)},
        {QStringLiteral("privateBytes"), static_cast<double>(privateBytes)},
        {QStringLiteral("virtualBytes"), static_cast<double>(virtualBytes)},
        {QStringLiteral("allocatorInUseBytes"), static_cast<double>(allocatorInUseBytes)},
        {QStringLiteral("allocatorReservedBytes"), static_cast<double>(allocatorReservedBytes)},
    };
    if (threadCount >= 0) {
        object[QStringLiteral("threadCount")] = static_cast<double>(threadCount);
    }
    if (handleCount >= 0) {
        object[QStringLiteral("handleCount")] = static_cast<double>(handleCount);
    }
    return object;
}

MemoryTelemetrySeries::MemoryTelemetrySeries(int maxSamples)
{
    setMaxSamples(maxSamples);
}

void MemoryTelemetrySeries::clear()
{
    m_points.clear();
    m_firstClasses.clear();
    m_lastClasses.clear();
}

void MemoryTelemetrySeries::setMaxSamples(int maxSamples)
{
    m_maxSamples = qBound(2, maxSamples, 10000);
    while (static_cast<int>(m_points.size()) > m_maxSamples) {
        m_points.pop_front();
    }
}

QMap<QString, qint64> MemoryTelemetrySeries::jsonCountMap(const QJsonObject& object)
{
    QMap<QString, qint64> counts;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isDouble()) {
            counts.insert(it.key(), static_cast<qint64>(it.value().toDouble()));
        }
    }
    return counts;
}

void MemoryTelemetrySeries::addSnapshot(qint64 elapsedMs, const QJsonObject& snapshot)
{
    Point point;
    point.elapsedMs = std::max<qint64>(0, elapsedMs);

    const QJsonObject process = snapshot.value(QStringLiteral("process")).toObject();
    for (const char* key : {"residentBytes", "peakResidentBytes", "privateBytes",
                            "virtualBytes", "allocatorInUseBytes",
                            "allocatorReservedBytes"}) {
        point.byteMetrics.insert(QStringLiteral("process.") + QString::fromLatin1(key),
                                 jsonNumber(process, key));
    }
    for (const char* key : {"threadCount", "handleCount"}) {
        if (process.contains(QString::fromLatin1(key))) {
            point.countMetrics.insert(QStringLiteral("process.") + QString::fromLatin1(key),
                                      jsonNumber(process, key));
        }
    }

    const QJsonObject subsystems = snapshot.value(QStringLiteral("subsystems")).toObject();
    for (auto it = subsystems.constBegin(); it != subsystems.constEnd(); ++it) {
        const QString prefix = QStringLiteral("subsystem.") + it.key() + QLatin1Char('.');
        const QJsonObject subsystem = it.value().toObject();
        for (const char* key : {"trackedBytes", "estimatedGpuBytes"}) {
            if (subsystem.contains(QString::fromLatin1(key))) {
                point.byteMetrics.insert(prefix + QString::fromLatin1(key),
                                         jsonNumber(subsystem, key));
            }
        }
        if (subsystem.contains(QStringLiteral("objectCount"))) {
            point.countMetrics.insert(prefix + QStringLiteral("objectCount"),
                                      jsonNumber(subsystem, "objectCount"));
        }

        const QMap<QString, qint64> classes =
            jsonCountMap(subsystem.value(QStringLiteral("classes")).toObject());
        if (!classes.isEmpty()) {
            if (!m_firstClasses.contains(it.key())) {
                m_firstClasses.insert(it.key(), classes);
            }
            m_lastClasses.insert(it.key(), classes);
        }
    }

    if (m_points.empty()) {
        m_points.push_back(std::move(point));
        return;
    }
    if (static_cast<int>(m_points.size()) >= m_maxSamples) {
        m_points.pop_front();
    }
    m_points.push_back(std::move(point));
}

QJsonObject MemoryTelemetrySeries::trend(const std::deque<Point>& points,
                                         const QString& metric,
                                         bool bytes)
{
    QVector<QPair<double, double>> values;
    values.reserve(static_cast<qsizetype>(points.size()));
    for (const Point& point : points) {
        const QMap<QString, double>& metrics = bytes
            ? point.byteMetrics : point.countMetrics;
        const auto found = metrics.constFind(metric);
        if (found != metrics.constEnd()) {
            values.append({point.elapsedMs / 3600000.0, found.value()});
        }
    }
    if (values.isEmpty()) {
        return {};
    }

    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();
    double sumX = 0.0;
    double sumY = 0.0;
    for (const auto& value : values) {
        sumX += value.first;
        sumY += value.second;
        minValue = std::min(minValue, value.second);
        maxValue = std::max(maxValue, value.second);
    }
    const double meanX = sumX / values.size();
    const double meanY = sumY / values.size();
    double covariance = 0.0;
    double varianceX = 0.0;
    double varianceY = 0.0;
    for (const auto& value : values) {
        const double dx = value.first - meanX;
        const double dy = value.second - meanY;
        covariance += dx * dy;
        varianceX += dx * dx;
        varianceY += dy * dy;
    }
    const double slopePerHour = varianceX > 0.0 ? covariance / varianceX : 0.0;
    const double rSquared = varianceX > 0.0 && varianceY > 0.0
        ? std::clamp((covariance * covariance) / (varianceX * varianceY), 0.0, 1.0)
        : 0.0;
    const double first = values.constFirst().second;
    const double last = values.constLast().second;
    const double delta = last - first;
    // Span of THIS metric's own samples (x is in hours), not the whole series —
    // a subsystem metric that only appears partway through the run must have its
    // sustainedGrowth duration gate measured over the range it was actually
    // sampled, not the full point range.
    const double durationMs =
        (values.constLast().first - values.constFirst().first) * 3600000.0;
    const double growthThreshold = bytes ? 4.0 * 1024.0 * 1024.0 : 1.0;
    const bool sustainedGrowth = values.size() >= 6
        && durationMs >= 60000.0
        && delta >= growthThreshold
        && slopePerHour > 0.0
        && rSquared >= 0.5;

    return QJsonObject{
        {QStringLiteral("samples"), values.size()},
        {QStringLiteral("first"), first},
        {QStringLiteral("last"), last},
        {QStringLiteral("delta"), delta},
        {QStringLiteral("min"), minValue},
        {QStringLiteral("max"), maxValue},
        {bytes ? QStringLiteral("slopeBytesPerHour")
               : QStringLiteral("slopePerHour"), slopePerHour},
        {QStringLiteral("rSquared"), rSquared},
        {QStringLiteral("sustainedGrowth"), sustainedGrowth},
    };
}

QJsonObject MemoryTelemetrySeries::classGrowth(
    const QMap<QString, QMap<QString, qint64>>& first,
    const QMap<QString, QMap<QString, qint64>>& last)
{
    QJsonObject result;
    for (auto subsystem = last.constBegin(); subsystem != last.constEnd(); ++subsystem) {
        const QMap<QString, qint64> baseline = first.value(subsystem.key());
        QJsonObject growth;
        for (auto klass = subsystem.value().constBegin();
             klass != subsystem.value().constEnd(); ++klass) {
            const qint64 delta = klass.value() - baseline.value(klass.key());
            if (delta != 0) {
                growth.insert(klass.key(), static_cast<double>(delta));
            }
        }
        for (auto klass = baseline.constBegin(); klass != baseline.constEnd(); ++klass) {
            if (!subsystem.value().contains(klass.key()) && klass.value() != 0) {
                growth.insert(klass.key(), static_cast<double>(-klass.value()));
            }
        }
        if (!growth.isEmpty()) {
            result.insert(subsystem.key(), growth);
        }
    }
    return result;
}

QJsonObject MemoryTelemetrySeries::pointToJson(const Point& point)
{
    QJsonObject bytes;
    for (auto it = point.byteMetrics.constBegin(); it != point.byteMetrics.constEnd(); ++it) {
        bytes.insert(it.key(), it.value());
    }
    QJsonObject counts;
    for (auto it = point.countMetrics.constBegin(); it != point.countMetrics.constEnd(); ++it) {
        counts.insert(it.key(), it.value());
    }
    return QJsonObject{
        {QStringLiteral("elapsedMs"), static_cast<double>(point.elapsedMs)},
        {QStringLiteral("bytes"), bytes},
        {QStringLiteral("counts"), counts},
    };
}

QJsonObject MemoryTelemetrySeries::report(bool includeSamples) const
{
    QJsonObject result{
        {QStringLiteral("sampleCount"), sampleCount()},
        {QStringLiteral("maxSamples"), m_maxSamples},
        {QStringLiteral("storageBytesEstimate"),
         static_cast<double>(estimatedStorageBytes())},
    };
    if (m_points.empty()) {
        result[QStringLiteral("durationMs")] = 0;
        return result;
    }

    result[QStringLiteral("durationMs")] =
        static_cast<double>(m_points.back().elapsedMs - m_points.front().elapsedMs);
    QSet<QString> byteNames;
    QSet<QString> countNames;
    for (const Point& point : m_points) {
        for (auto it = point.byteMetrics.constBegin(); it != point.byteMetrics.constEnd(); ++it) {
            byteNames.insert(it.key());
        }
        for (auto it = point.countMetrics.constBegin(); it != point.countMetrics.constEnd(); ++it) {
            countNames.insert(it.key());
        }
    }

    QJsonObject byteTrends;
    for (const QString& name : byteNames) {
        byteTrends.insert(name, trend(m_points, name, true));
    }
    QJsonObject countTrends;
    for (const QString& name : countNames) {
        countTrends.insert(name, trend(m_points, name, false));
    }
    result[QStringLiteral("byteTrends")] = byteTrends;
    result[QStringLiteral("countTrends")] = countTrends;
    result[QStringLiteral("classCountGrowth")] =
        classGrowth(m_firstClasses, m_lastClasses);

    QJsonArray suspects;
    for (auto it = byteTrends.constBegin(); it != byteTrends.constEnd(); ++it) {
        const QJsonObject metric = it.value().toObject();
        if (metric.value(QStringLiteral("sustainedGrowth")).toBool()) {
            suspects.append(QJsonObject{
                {QStringLiteral("metric"), it.key()},
                {QStringLiteral("deltaBytes"), metric.value(QStringLiteral("delta"))},
                {QStringLiteral("slopeBytesPerHour"),
                 metric.value(QStringLiteral("slopeBytesPerHour"))},
                {QStringLiteral("rSquared"), metric.value(QStringLiteral("rSquared"))},
            });
        }
    }
    result[QStringLiteral("growthSuspects")] = suspects;

    if (includeSamples) {
        QJsonArray samples;
        for (const Point& point : m_points) {
            samples.append(pointToJson(point));
        }
        result[QStringLiteral("samples")] = samples;
    }
    return result;
}

quint64 MemoryTelemetrySeries::estimatedStorageBytes() const
{
    quint64 bytes = static_cast<quint64>(m_points.size()) * sizeof(Point);
    for (const Point& point : m_points) {
        bytes += static_cast<quint64>(point.byteMetrics.size()
            + point.countMetrics.size()) * 96ULL;
    }
    for (auto subsystem = m_lastClasses.constBegin();
         subsystem != m_lastClasses.constEnd(); ++subsystem) {
        bytes += static_cast<quint64>(subsystem.value().size()) * 96ULL;
    }
    return bytes;
}

} // namespace AetherSDR
