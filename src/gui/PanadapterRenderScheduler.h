#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include "WindowRepaintScheduler.h"

class QWidget;

namespace AetherSDR {

class SpectrumWidget;

class PanadapterRenderScheduler : public QObject {
public:
    explicit PanadapterRenderScheduler(QObject* parent = nullptr);

    void requestDataFrame(SpectrumWidget* widget, int slotMs);
    QVariantMap statsSnapshot(bool reset);

private:
    void scheduleFlush(int delayMs);
    void flushPending();
    bool markDirty(SpectrumWidget* widget);

    QTimer m_flushTimer;
    QElapsedTimer m_presentClock;
    QVector<QPointer<SpectrumWidget>> m_dirtyWidgets;
    // Top-level window -> its scheduler; dies with the window, pruned on miss.
    QHash<const QWidget*, QPointer<WindowRepaintScheduler>> m_windowSchedulers;

    quint64 m_requests{0};
    quint64 m_coalescedRequests{0};
    quint64 m_flushes{0};
    quint64 m_zeroDelayFlushes{0};
    quint64 m_timerFlushes{0};
    quint64 m_widgetsUpdated{0};
    quint64 m_deadWidgets{0};
    quint64 m_timerShortens{0};
    int m_lastSlotMs{0};
    int m_lastDelayMs{-1};
    int m_lastFlushWidgets{0};
    int m_maxDirtyPerFlush{0};
};

} // namespace AetherSDR
