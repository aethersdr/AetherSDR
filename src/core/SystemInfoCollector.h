#pragma once

#include "SystemInfo.h"

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

class QTimer;

namespace AetherSDR {

// One reading of this process's memory, taken on the collector's tick and
// carried to the GUI by value (#2554, the Memory tab). A flat copy of the
// fields ProcessMemorySnapshot::capture() fills, so the dialog needs neither
// MemoryTelemetry.h nor a JSON round-trip to draw a chart. residentMetric names
// what "resident" means on this platform (physicalFootprint / workingSet /
// vmRss / unsupported) — the number is only honest with its name beside it.
struct MemorySample {
    qint64  wallMs{0};              // QDateTime::currentMSecsSinceEpoch() at capture
    bool    valid{false};           // false = this platform reported nothing usable
    QString residentMetric;
    quint64 residentBytes{0};
    quint64 peakResidentBytes{0};
    quint64 privateBytes{0};
    quint64 virtualBytes{0};
};

// One process-level CPU reading per tick for the Overview tab (#2554).
// processPercentOfCapacity comes from SystemInfo::processCpuUsecs over the
// interval, not from summing per-thread samples (exited threads vanish from
// the sum). busiest*/busyThreads come from the per-thread samples; busyThreads
// lists every thread with a non-zero share this tick, which stays compact so
// the history ring can hold an hour and pick Top Threads over the window.
struct CpuSample {
    qint64  wallMs{0};                       // QDateTime::currentMSecsSinceEpoch() at capture
    int     coreCount{0};                    // QThread::idealThreadCount() — the footer's divisor
    double  processPercentOfCapacity{0.0};   // 0..100 of the whole machine (whole-process counter)
    // false = no per-thread reading on this tick; the three busiest* fields
    // below are then defaults, not a measurement of an unnamed thread at 0 %.
    bool    hasBusiest{false};
    quint64 busiestTid{0};
    QString busiestName;                     // empty when the busiest thread has no name
    double  busiestPercentOfCore{0.0};       // 0..100 of one core
    QVector<ThreadCpuSample> busyThreads;    // cpuPercentOfCore > 0 only
};

// Samples per-thread CPU and process memory on a worker thread for the GUI
// (#2554), off the GUI thread so the collector isn't measuring or causing the
// stall being investigated. Parentless, moved onto a QThread by its owner with
// init() connected to QThread::started (like FlexBackend's workers), so the
// owner decides its lifetime.
class SystemInfoCollector : public QObject {
    Q_OBJECT

public:
    // The cadence the issue asks for, matching the status bar's existing
    // CPU/Mem refresh so the two surfaces cannot disagree about "now".
    static constexpr int kSampleIntervalMs = 1500;

    // Acceptance criterion 3, verbatim: "Max-thread % alerts when any single
    // thread exceeds 90% of one core". Maintainer-authored and quoted here so
    // the number's provenance travels with it — unlike the Overview card
    // thresholds, which are invented values still awaiting a measured session.
    static constexpr double kMaxThreadPercentOfCore = 90.0;

    explicit SystemInfoCollector(QObject* parent = nullptr);

public slots:
    // Connect to QThread::started. Creates the timer HERE rather than in the
    // constructor: a QTimer belongs to the thread that started it, and one
    // created on the GUI thread would fire there no matter which thread owns
    // this object.
    void init();

    // Stop and destroy the timer ON THE THREAD THAT CREATED IT. A QTimer belongs
    // to its owning thread; deleting the collector from the GUI thread after the
    // worker has exited destroys a timer whose thread is gone, which Qt reports
    // as "Timers cannot be stopped from another thread" and is undefined
    // behaviour. Invoke this with a blocking queued connection before quit().
    void shutdown();

signals:
    // Queued to the GUI thread by Qt, since emitter and receiver differ.
    void sampleReady(const QVector<AetherSDR::ThreadCpuSample>& threads);

    // The crossing, not the condition: emitted once when the busiest thread
    // goes above kMaxThreadPercentOfCore, and not again until it has come back
    // down. A thread pinned at 95 % for a minute is one event, not forty.
    //
    // Separate from sampleReady because it is the seam the analysis asked for
    // ("cheap to add now; expensive to retrofit later"): what an alert should
    // LOOK like is still open on the issue, and a consumer that wants a badge
    // or a toast attaches here without the collector having to know.
    void thresholdExceeded(const QString& threadName, double percentOfCore);

    // Process memory on every tick, including the first: unlike the CPU
    // percentages it needs no previous sample to be meaningful, so the Memory
    // tab shows a point 1.5 s after the dialog opens rather than 3 s. Queued
    // to the GUI thread like sampleReady.
    void memorySampleReady(const AetherSDR::MemorySample& sample);

    // The Overview tab's reading, emitted right after sampleReady on every tick
    // that has an interval to report — so a consumer listening to both sees
    // the table and the cards agree about "now". Queued like the others.
    void cpuSampleReady(const AetherSDR::CpuSample& sample);

private:
    void sampleOnce();

    QTimer* m_timer{nullptr};
    QVector<ThreadTimes> m_previous;   // last raw snapshot, for the per-thread deltas
    std::optional<quint64> m_previousProcessCpuUsecs;   // whole-process counter at the last snapshot
    QElapsedTimer m_sinceLastSample;
    double m_previousBusiestPercent{0.0};   // the latch behind thresholdExceeded
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::MemorySample)
Q_DECLARE_METATYPE(AetherSDR::CpuSample)
