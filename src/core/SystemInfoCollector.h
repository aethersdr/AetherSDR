#pragma once

#include "SystemInfo.h"

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

class QTimer;

namespace AetherSDR {

// Samples per-thread CPU on a worker thread and publishes the result to the GUI
// (#2554).
//
// Shaped like the other workers in this codebase rather than as a self-owning
// thread: the object is parentless, moved onto a QThread by its owner, and
// init() is connected to QThread::started — the same wiring FlexBackend uses for
// PanadapterStream and RadioConnection. That keeps the lifetime decision at the
// call site, which matters while it is still open whether sampling should run
// only while the dialog is visible.
//
// Sampling off the GUI thread is the point: enumerating every thread is the
// kind of work that would otherwise be measured by the very metric it is
// gathering, and a stall in the collector would be indistinguishable from the
// stall an operator opened the dialog to investigate.
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

private:
    void sampleOnce();

    QTimer* m_timer{nullptr};
    QVector<ThreadTimes> m_previous;   // last raw snapshot, for the delta
    QElapsedTimer m_sinceLastSample;
    double m_previousBusiestPercent{0.0};   // the latch behind thresholdExceeded
};

}  // namespace AetherSDR
