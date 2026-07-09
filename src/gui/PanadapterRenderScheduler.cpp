#include "PanadapterRenderScheduler.h"

#include "SpectrumWidget.h"

#include <QMetaObject>
#include <QThread>

#include <algorithm>

namespace AetherSDR {

PanadapterRenderScheduler::PanadapterRenderScheduler(QObject* parent)
    : QObject(parent)
{
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this,
            [this]() { flushPending(); });
}

void PanadapterRenderScheduler::requestDataFrame(SpectrumWidget* widget, int slotMs)
{
    if (!widget) {
        return;
    }

    if (QThread::currentThread() != thread()) {
        QPointer<SpectrumWidget> guard(widget);
        QMetaObject::invokeMethod(this, [this, guard, slotMs]() {
            if (guard) {
                requestDataFrame(guard, slotMs);
            }
        }, Qt::QueuedConnection);
        return;
    }

    ++m_requests;
    slotMs = std::max(1, slotMs);
    m_lastSlotMs = slotMs;

    if (!markDirty(widget)) {
        ++m_coalescedRequests;
    }

    int delayMs = 0;
    if (!m_presentClock.isValid()) {
        m_presentClock.start();
    } else {
        const qint64 sinceMs = m_presentClock.elapsed();
        if (sinceMs < slotMs) {
            delayMs = static_cast<int>(slotMs - sinceMs);
        }
    }

    scheduleFlush(delayMs);
}

QVariantMap PanadapterRenderScheduler::statsSnapshot(bool reset)
{
    QVariantMap stats;
    stats[QStringLiteral("enabled")] = true;
    stats[QStringLiteral("requests")] = static_cast<qulonglong>(m_requests);
    stats[QStringLiteral("coalescedRequests")] =
        static_cast<qulonglong>(m_coalescedRequests);
    stats[QStringLiteral("flushes")] = static_cast<qulonglong>(m_flushes);
    stats[QStringLiteral("zeroDelayFlushes")] =
        static_cast<qulonglong>(m_zeroDelayFlushes);
    stats[QStringLiteral("timerFlushes")] = static_cast<qulonglong>(m_timerFlushes);
    stats[QStringLiteral("widgetsUpdated")] =
        static_cast<qulonglong>(m_widgetsUpdated);
    stats[QStringLiteral("deadWidgets")] = static_cast<qulonglong>(m_deadWidgets);
    stats[QStringLiteral("timerShortens")] =
        static_cast<qulonglong>(m_timerShortens);
    stats[QStringLiteral("pendingWidgets")] = m_dirtyWidgets.size();
    stats[QStringLiteral("timerActive")] = m_flushTimer.isActive();
    stats[QStringLiteral("timerRemainingMs")] = m_flushTimer.remainingTime();
    stats[QStringLiteral("lastSlotMs")] = m_lastSlotMs;
    stats[QStringLiteral("lastDelayMs")] = m_lastDelayMs;
    stats[QStringLiteral("lastFlushWidgets")] = m_lastFlushWidgets;
    stats[QStringLiteral("maxDirtyPerFlush")] = m_maxDirtyPerFlush;
    stats[QStringLiteral("avgWidgetsPerFlush")] = m_flushes == 0
        ? 0.0
        : static_cast<double>(m_widgetsUpdated) / static_cast<double>(m_flushes);

    if (reset) {
        m_requests = 0;
        m_coalescedRequests = 0;
        m_flushes = 0;
        m_zeroDelayFlushes = 0;
        m_timerFlushes = 0;
        m_widgetsUpdated = 0;
        m_deadWidgets = 0;
        m_timerShortens = 0;
        m_lastSlotMs = 0;
        m_lastDelayMs = -1;
        m_lastFlushWidgets = 0;
        m_maxDirtyPerFlush = 0;
    }

    return stats;
}

void PanadapterRenderScheduler::scheduleFlush(int delayMs)
{
    delayMs = std::max(0, delayMs);
    const int remainingMs = m_flushTimer.remainingTime();
    if (remainingMs >= 0 && remainingMs <= delayMs) {
        return;
    }

    if (remainingMs > delayMs) {
        ++m_timerShortens;
    }

    m_lastDelayMs = delayMs;
    m_flushTimer.start(delayMs);
}

void PanadapterRenderScheduler::flushPending()
{
    QVector<QPointer<SpectrumWidget>> dirty;
    dirty.swap(m_dirtyWidgets);

    if (!m_presentClock.isValid()) {
        m_presentClock.start();
    } else {
        m_presentClock.restart();
    }

    ++m_flushes;
    if (m_lastDelayMs == 0) {
        ++m_zeroDelayFlushes;
    } else {
        ++m_timerFlushes;
    }

    m_lastFlushWidgets = dirty.size();
    m_maxDirtyPerFlush = std::max(m_maxDirtyPerFlush, m_lastFlushWidgets);

    for (const QPointer<SpectrumWidget>& widget : dirty) {
        if (!widget) {
            ++m_deadWidgets;
            continue;
        }
        widget->update();
        ++m_widgetsUpdated;
    }

    m_lastDelayMs = -1;
}

bool PanadapterRenderScheduler::markDirty(SpectrumWidget* widget)
{
    for (const QPointer<SpectrumWidget>& dirty : m_dirtyWidgets) {
        if (dirty.data() == widget) {
            return false;
        }
    }

    m_dirtyWidgets.append(widget);
    return true;
}

} // namespace AetherSDR
