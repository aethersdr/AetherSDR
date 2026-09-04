#include "SystemInfoCollector.h"

#include "MemoryTelemetry.h"
#include "ThreadName.h"

#include <QDateTime>
#include <QMetaType>
#include <QThread>
#include <QTimer>

namespace AetherSDR {

SystemInfoCollector::SystemInfoCollector(QObject* parent)
    : QObject(parent)
{
    // A queued connection copies the argument through the metatype system, so
    // the payload type has to be registered or the emit is silently dropped
    // with a runtime warning rather than failing to compile.
    qRegisterMetaType<QVector<AetherSDR::ThreadCpuSample>>("QVector<AetherSDR::ThreadCpuSample>");
    qRegisterMetaType<AetherSDR::MemorySample>("AetherSDR::MemorySample");
    qRegisterMetaType<AetherSDR::CpuSample>("AetherSDR::CpuSample");
}

void SystemInfoCollector::init()
{
    if (m_timer != nullptr) {
        return;  // already running; init() can arrive again after a restart
    }

    // The sampler is itself a thread in the table it produces. Naming it means
    // an operator can see the observer's own cost rather than wondering which
    // unnamed row it is.
    setCurrentThreadName("SystemInfoCollector");

    // Seed the baseline so the first published sample is a real interval rather
    // than every thread's whole lifetime divided by a few milliseconds.
    m_previous = SystemInfo::enumerateThreads();
    m_sinceLastSample.start();

    m_timer = new QTimer(this);
    m_timer->setInterval(kSampleIntervalMs);
    m_timer->setTimerType(Qt::CoarseTimer);  // a diagnostic, not a deadline
    connect(m_timer, &QTimer::timeout, this, &SystemInfoCollector::sampleOnce);
    m_timer->start();
}

void SystemInfoCollector::shutdown()
{
    if (m_timer != nullptr) {
        m_timer->stop();
        delete m_timer;      // on the worker thread, where it was created
        m_timer = nullptr;
    }
    m_previous.clear();
    // Reset with the rest of the state: a collector restarted after the dialog
    // was hidden would otherwise inherit "already above the threshold" and
    // swallow the next crossing.
    m_previousBusiestPercent = 0.0;
}

namespace {

MemorySample memorySampleNow()
{
    // One task_info / /proc read / GetProcessMemoryInfo per tick — the same
    // call the automation bridge's memory profile makes, cheap enough for
    // 1.5 s and taken here, off the GUI thread, for the same reason the thread
    // table is.
    const ProcessMemorySnapshot snapshot = ProcessMemorySnapshot::capture();
    MemorySample sample;
    sample.wallMs = QDateTime::currentMSecsSinceEpoch();
    sample.valid = snapshot.valid;
    sample.residentMetric = snapshot.residentMetric;
    sample.residentBytes = snapshot.residentBytes;
    sample.peakResidentBytes = snapshot.peakResidentBytes;
    sample.privateBytes = snapshot.privateBytes;
    sample.virtualBytes = snapshot.virtualBytes;
    return sample;
}

}  // namespace

void SystemInfoCollector::sampleOnce()
{
    // Before the thread enumeration and its early return: a memory reading
    // does not depend on the thread table, so a platform whose enumeration
    // fails still gets a Memory tab.
    emit memorySampleReady(memorySampleNow());

    const QVector<ThreadTimes> current = SystemInfo::enumerateThreads();
    if (current.isEmpty()) {
        return;  // enumeration failed; publishing an empty table would read as "no threads"
    }

    // Measured elapsed time, not the nominal interval: a late timer would
    // otherwise inflate every percentage by the amount the sampler itself was
    // delayed — precisely the case a busy machine produces, and precisely when
    // the numbers need to be trustworthy.
    const qint64 elapsedUsecs = m_sinceLastSample.nsecsElapsed() / 1000;
    m_sinceLastSample.restart();

    if (!m_previous.isEmpty() && elapsedUsecs > 0) {
        const QVector<ThreadCpuSample> samples = SystemInfo::cpuPercentBetween(
            m_previous, current, static_cast<quint64>(elapsedUsecs));
        emit sampleReady(samples);

        // The crossing is evaluated here rather than by the consumer so that
        // every consumer sees the same event. The latch lives in this object,
        // which is the only thing that sees every sample — a dialog that was
        // hidden for a minute would otherwise re-announce a condition that had
        // been true the whole time.
        const int busiest = SystemInfo::busiestThreadIndex(samples);
        if (busiest >= 0) {
            const double percent = samples.at(busiest).cpuPercentOfCore;
            if (SystemInfo::crossedThreshold(m_previousBusiestPercent, percent,
                                             kMaxThreadPercentOfCore)) {
                emit thresholdExceeded(samples.at(busiest).name, percent);
            }
            m_previousBusiestPercent = percent;
        }

        // The Overview's reading, from the same samples the table just got.
        CpuSample cpu;
        cpu.wallMs = QDateTime::currentMSecsSinceEpoch();
        cpu.coreCount = QThread::idealThreadCount();
        cpu.processPercentOfCapacity = SystemInfo::processPercentOfCapacity(samples, cpu.coreCount);
        if (busiest >= 0) {
            cpu.busiestTid = samples.at(busiest).tid;
            cpu.busiestName = samples.at(busiest).name;
            cpu.busiestPercentOfCore = samples.at(busiest).cpuPercentOfCore;
        }
        for (const ThreadCpuSample& sample : samples) {
            if (sample.cpuPercentOfCore > 0.0) {
                cpu.busyThreads.push_back(sample);
            }
        }
        emit cpuSampleReady(cpu);
    }
    m_previous = current;
}

}  // namespace AetherSDR
