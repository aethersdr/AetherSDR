#include "PerfTelemetry.h"
#include "LogManager.h"

#include <QLoggingCategory>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace AetherSDR {

namespace {

constexpr qint64 kSummaryIntervalNs = 1000000000LL;
constexpr double kHeartbeatIntervalMs = 50.0;
constexpr double kUiLagStallMs = 50.0;
constexpr double kDragUiLagStallMs = 33.0;
constexpr double kFrameAgeStallMs = 100.0;
constexpr double kPanUpdateStallMs = 8.0;
constexpr double kWaterfallUpdateStallMs = 12.0;
constexpr double kRenderStallMs = 16.0;
constexpr double kUdpDrainStallMs = 8.0;
constexpr double kInputStallMs = 16.0;

QString frameKindName(PerfTelemetry::FrameKind kind)
{
    return kind == PerfTelemetry::FrameKind::Waterfall
        ? QStringLiteral("waterfall")
        : QStringLiteral("panadapter");
}

} // namespace

PerfTelemetry& PerfTelemetry::instance()
{
    static PerfTelemetry telemetry;
    return telemetry;
}

qint64 PerfTelemetry::nowNs()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

bool PerfTelemetry::enabled() const
{
    return lcPerf().isDebugEnabled();
}

bool PerfTelemetry::beginRecord(qint64 now)
{
    if (!enabled()) {
        m_wasEnabled.store(false, std::memory_order_relaxed);
        return false;
    }

    if (!m_wasEnabled.exchange(true, std::memory_order_relaxed)) {
        QMutexLocker lock(&m_mutex);
        resetWindowLocked(now);
        m_lastHeartbeatNs = 0;
    }

    return true;
}

void PerfTelemetry::resetWindowLocked(qint64 now)
{
    m_window = Window{};
    m_window.dragSeen = m_dragActive.load(std::memory_order_relaxed);
    m_windowStartNs = now;
}

double PerfTelemetry::nsToMs(qint64 ns)
{
    return static_cast<double>(ns) / 1000000.0;
}

QString PerfTelemetry::msValue(double value)
{
    return QString::number(value, 'f', 1);
}

QString PerfTelemetry::pctValue(int errors, int packets)
{
    if (packets <= 0)
        return QStringLiteral("0.00");
    return QString::number((100.0 * static_cast<double>(errors)) / static_cast<double>(packets), 'f', 2);
}

QString PerfTelemetry::keyValue(const QString& key, const QString& value)
{
    return QStringLiteral("%1=%2").arg(key, value);
}

QString PerfTelemetry::keyValue(const QString& key, int value)
{
    return QStringLiteral("%1=%2").arg(key).arg(value);
}

QString PerfTelemetry::keyValue(const QString& key, qint64 value)
{
    return QStringLiteral("%1=%2").arg(key).arg(value);
}

QString PerfTelemetry::keyValueMs(const QString& key, double value)
{
    return keyValue(key, msValue(value));
}

double PerfTelemetry::percentile95(QVector<double> values)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const int index = qBound(0, static_cast<int>(std::ceil(values.size() * 0.95)) - 1, values.size() - 1);
    return values.at(index);
}

void PerfTelemetry::logStall(const QStringList& fields)
{
    if (!lcPerf().isDebugEnabled())
        return;
    QStringList line;
    line.reserve(fields.size() + 1);
    line << QStringLiteral("PerfStall");
    line << fields;
    qCDebug(lcPerf).noquote() << line.join(QLatin1Char(' '));
}

void PerfTelemetry::recordUdpBatch(int datagrams, qint64 bytes, double drainMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.udpBatches++;
        m_window.udpDatagrams += datagrams;
        m_window.udpBytes += bytes;
        m_window.udpMaxDatagramsPerBatch = std::max(m_window.udpMaxDatagramsPerBatch, datagrams);
        m_window.udpDrainMaxMs = std::max(m_window.udpDrainMaxMs, drainMs);
    }

    if (drainMs > kUdpDrainStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("udpDrain")),
            keyValue(QStringLiteral("drainMs"), msValue(drainMs)),
            keyValue(QStringLiteral("datagrams"), datagrams),
            keyValue(QStringLiteral("bytes"), bytes)
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordStreamPacket(FrameKind kind, bool sequenceError)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        if (kind == FrameKind::Waterfall) {
            m_window.waterfallPackets++;
            if (sequenceError)
                m_window.waterfallErrors++;
        } else {
            m_window.fftPackets++;
            if (sequenceError)
                m_window.fftErrors++;
        }
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordFrameRestart(FrameKind kind)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        if (kind == FrameKind::Waterfall)
            m_window.waterfallFrameRestarts++;
        else
            m_window.fftFrameRestarts++;
    }

    logStall({
        keyValue(QStringLiteral("kind"), QStringLiteral("frameRestart")),
        keyValue(QStringLiteral("stream"), frameKindName(kind))
    });

    maybeLogSummary(now);
}

void PerfTelemetry::recordFrameAge(FrameKind kind, double ageMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        QVector<double>& samples = kind == FrameKind::Waterfall
            ? m_window.waterfallAgeMs
            : m_window.panAgeMs;
        samples.append(ageMs);
    }

    if (ageMs > kFrameAgeStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("frameAge")),
            keyValue(QStringLiteral("stream"), frameKindName(kind)),
            keyValue(QStringLiteral("ageMs"), msValue(ageMs))
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordPanFrame()
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.panFrames++;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordWaterfallNativeRows(int rows)
{
    if (rows <= 0)
        return;
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.waterfallFrames += rows;
        m_window.waterfallNativeRows += rows;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordWaterfallFallbackRows(int rows)
{
    if (rows <= 0)
        return;
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.waterfallFrames += rows;
        m_window.waterfallFallbackRows += rows;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordWaterfallVisibleRows(int rows)
{
    if (rows <= 0)
        return;
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.waterfallVisibleRows += rows;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordWaterfallRebuild()
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.waterfallRebuilds++;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::setWaterfallLineDurationMs(int ms)
{
    m_waterfallLineDurationMs.store(ms, std::memory_order_relaxed);
}

void PerfTelemetry::recordPanCenterCommand()
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.panCenterCommands++;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordPanUpdate(double updateMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.panUpdateMs.append(updateMs);
    }

    if (updateMs > kPanUpdateStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("updateSpectrum")),
            keyValue(QStringLiteral("durationMs"), msValue(updateMs))
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordWaterfallUpdate(double updateMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.waterfallUpdateMs.append(updateMs);
    }

    if (updateMs > kWaterfallUpdateStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("updateWaterfallRow")),
            keyValue(QStringLiteral("durationMs"), msValue(updateMs))
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordRender(double renderMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.renderMs.append(renderMs);
    }

    if (renderMs > kRenderStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("render")),
            keyValue(QStringLiteral("durationMs"), msValue(renderMs))
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordGpuUpload(GpuUploadKind kind)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.gpuUploads++;
        switch (kind) {
        case GpuUploadKind::WaterfallFull:
            m_window.gpuWaterfallFullUploads++;
            break;
        case GpuUploadKind::WaterfallIncremental:
            m_window.gpuWaterfallIncrementalUploads++;
            break;
        case GpuUploadKind::Overlay:
            m_window.gpuOverlayUploads++;
            break;
        }
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordUiHeartbeat()
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    double lagMs = 0.0;
    const bool dragActive = m_dragActive.load(std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_mutex);
        if (m_lastHeartbeatNs > 0) {
            const double gapMs = nsToMs(now - m_lastHeartbeatNs);
            lagMs = std::max(0.0, gapMs - kHeartbeatIntervalMs);
            m_window.uiLagMaxMs = std::max(m_window.uiLagMaxMs, lagMs);
        }
        m_lastHeartbeatNs = now;
        m_window.dragSeen = m_window.dragSeen || dragActive;
    }

    if (lagMs > kUiLagStallMs || (dragActive && lagMs > kDragUiLagStallMs)) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("uiHeartbeat")),
            keyValue(QStringLiteral("lagMs"), msValue(lagMs)),
            keyValue(QStringLiteral("drag"), dragActive ? 1 : 0)
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::setDragActive(bool active)
{
    m_dragActive.store(active, std::memory_order_relaxed);

    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.dragSeen = m_window.dragSeen || active;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordMouseMoveGap(double gapMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.mouseMoveGapMaxMs = std::max(m_window.mouseMoveGapMaxMs, gapMs);
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordInputEvent(const char* kind, double durationMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.inputMs.append(durationMs);
    }

    if (durationMs > kInputStallMs) {
        logStall({
            keyValue(QStringLiteral("kind"), QStringLiteral("input")),
            keyValue(QStringLiteral("event"), QString::fromLatin1(kind ? kind : "unknown")),
            keyValue(QStringLiteral("durationMs"), msValue(durationMs)),
            keyValue(QStringLiteral("drag"), m_dragActive.load(std::memory_order_relaxed) ? 1 : 0)
        });
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordSHistoryProcessed(double durationMs)
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.shistoryProcessed++;
        m_window.shistoryMs.append(durationMs);
    }

    maybeLogSummary(now);
}

void PerfTelemetry::recordSHistorySkipped()
{
    const qint64 now = nowNs();
    if (!beginRecord(now))
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_window.shistorySkipped++;
    }

    maybeLogSummary(now);
}

void PerfTelemetry::maybeLogSummary(qint64 now)
{
    Window window;
    qint64 elapsedNs = 0;
    bool shouldLog = false;

    {
        QMutexLocker lock(&m_mutex);
        if (m_windowStartNs == 0)
            m_windowStartNs = now;

        elapsedNs = now - m_windowStartNs;
        if (elapsedNs < kSummaryIntervalNs)
            return;

        window = std::move(m_window);
        window.dragSeen = window.dragSeen || m_dragActive.load(std::memory_order_relaxed);
        resetWindowLocked(now);
        shouldLog = true;
    }

    if (!shouldLog)
        return;

    const double seconds = std::max(0.001, static_cast<double>(elapsedNs) / 1000000000.0);
    const double panFps = static_cast<double>(window.panFrames) / seconds;
    const double panCenterCommandRate = static_cast<double>(window.panCenterCommands) / seconds;
    const double waterfallFps = static_cast<double>(window.waterfallFrames) / seconds;
    const double rxKbps = (static_cast<double>(window.udpBytes) * 8.0) / seconds / 1000.0;
    const int waterfallLineDurationMs = m_waterfallLineDurationMs.load(std::memory_order_relaxed);

    QStringList fields;
    fields.reserve(45);
    fields << QStringLiteral("PerfSummary")
           << keyValueMs(QStringLiteral("uiLagMaxMs"), window.uiLagMaxMs)
           << keyValue(QStringLiteral("drag"), window.dragSeen ? 1 : 0)
           << keyValue(QStringLiteral("panFps"), QString::number(panFps, 'f', 1))
           << keyValue(QStringLiteral("panCenterCmds"), window.panCenterCommands)
           << keyValue(QStringLiteral("panCenterCmdRate"), QString::number(panCenterCommandRate, 'f', 1))
           << keyValueMs(QStringLiteral("panAgeP95Ms"), percentile95(std::move(window.panAgeMs)))
           << keyValueMs(QStringLiteral("panUpdateP95Ms"), percentile95(std::move(window.panUpdateMs)))
           << keyValue(QStringLiteral("wfFps"), QString::number(waterfallFps, 'f', 1))
           << keyValue(QStringLiteral("wfLineMs"), waterfallLineDurationMs)
           << keyValueMs(QStringLiteral("wfAgeP95Ms"), percentile95(std::move(window.waterfallAgeMs)))
           << keyValueMs(QStringLiteral("wfUpdateP95Ms"), percentile95(std::move(window.waterfallUpdateMs)))
           << keyValueMs(QStringLiteral("renderP95Ms"), percentile95(std::move(window.renderMs)))
           << keyValue(QStringLiteral("gpuUploads"), window.gpuUploads)
           << keyValue(QStringLiteral("gpuWfFull"), window.gpuWaterfallFullUploads)
           << keyValue(QStringLiteral("gpuWfRows"), window.gpuWaterfallIncrementalUploads)
           << keyValue(QStringLiteral("gpuOverlay"), window.gpuOverlayUploads)
           << keyValue(QStringLiteral("udpBatches"), window.udpBatches)
           << keyValue(QStringLiteral("udpDgrams"), window.udpDatagrams)
           << keyValue(QStringLiteral("udpBatchMax"), window.udpMaxDatagramsPerBatch)
           << keyValueMs(QStringLiteral("udpDrainMaxMs"), window.udpDrainMaxMs)
           << keyValue(QStringLiteral("rxKbps"), QString::number(rxKbps, 'f', 1))
           << keyValue(QStringLiteral("fftLossPct"), pctValue(window.fftErrors, window.fftPackets))
           << keyValue(QStringLiteral("wfLossPct"), pctValue(window.waterfallErrors, window.waterfallPackets))
           << keyValueMs(QStringLiteral("shistoryP95Ms"), percentile95(std::move(window.shistoryMs)))
           << keyValue(QStringLiteral("shistoryProcessed"), window.shistoryProcessed)
           << keyValue(QStringLiteral("shistorySkipped"), window.shistorySkipped)
           << keyValue(QStringLiteral("wfNative"), window.waterfallNativeRows)
           << keyValue(QStringLiteral("wfFallback"), window.waterfallFallbackRows)
           << keyValue(QStringLiteral("wfRebuilds"), window.waterfallRebuilds)
           << keyValue(QStringLiteral("wfVisibleRows"), window.waterfallVisibleRows)
           << keyValueMs(QStringLiteral("mouseGapMaxMs"), window.mouseMoveGapMaxMs)
           << keyValueMs(QStringLiteral("inputP95Ms"), percentile95(std::move(window.inputMs)))
           << keyValue(QStringLiteral("fftRestarts"), window.fftFrameRestarts)
           << keyValue(QStringLiteral("wfRestarts"), window.waterfallFrameRestarts);

    qCDebug(lcPerf).noquote() << fields.join(QLatin1Char(' '));
}

} // namespace AetherSDR
