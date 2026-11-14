#include "WindowRepaintScheduler.h"

#include <QCoreApplication>
#include <QEvent>
#include <QScreen>
#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace AetherSDR {

namespace {
std::optional<bool> s_processOverride;
}

WindowRepaintScheduler* WindowRepaintScheduler::ensureFor(QWidget* window, bool initiallyEnabled)
{
    if (!window) { return nullptr; }
    window = window->window();
    auto* existing = window->findChild<WindowRepaintScheduler*>(
        QStringLiteral("windowRepaintScheduler"), Qt::FindDirectChildrenOnly);
    return existing ? existing
                    : new WindowRepaintScheduler(window, s_processOverride.value_or(initiallyEnabled));
}

WindowRepaintScheduler::WindowRepaintScheduler(QWidget* window, bool initiallyEnabled)
    : QObject(window), m_window(window), m_enabled(initiallyEnabled)
{
    setObjectName(QStringLiteral("windowRepaintScheduler"));
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this,
            &WindowRepaintScheduler::deliverPending);
    m_statsClock.start();
    window->installEventFilter(this);
}

qint64 WindowRepaintScheduler::frameIntervalNs() const
{
    const QScreen* screen = m_window->screen();
    const double refresh = screen ? screen->refreshRate() : 60.0;
    // Unknown/invalid reports use 60 Hz. Read each request so moving between
    // monitors changes cadence without recreating the widget or its QRhi.
    const double hz = std::isfinite(refresh) && refresh >= 1.0 ? refresh : 60.0;
    return std::max<qint64>(1, std::llround(1.0e9 / hz));
}

bool WindowRepaintScheduler::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_window || event->type() != QEvent::UpdateRequest || m_delivering) {
        return false;
    }
    ++m_requests;
    const qint64 remaining = m_lastDelivery.isValid()
        ? frameIntervalNs() - m_lastDelivery.nsecsElapsed() : 0;
    if (!m_enabled || remaining <= 0) {
        m_timer.stop();
        m_pending = false;
        m_lastDelivery.start();
        ++m_deliveries;
        return false;
    }

    ++m_deferred;
    m_pending = true;
    if (!m_timer.isActive()) {
        // Round up: a timer must not fire before this window's next frame.
        m_timer.start(std::chrono::milliseconds((remaining + 999999) / 1000000));
    }
    return true;
}

void WindowRepaintScheduler::deliverPending()
{
    if (!m_pending) { return; }
    m_timer.stop();
    m_pending = false;
    m_lastDelivery.start();
    ++m_deliveries;
    m_delivering = true;
    QPointer<WindowRepaintScheduler> guard(this);
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(m_window, &update);
    if (guard) { m_delivering = false; }
}

void WindowRepaintScheduler::setEnabled(bool enabled)
{
    if (m_enabled == enabled) { return; }
    m_enabled = enabled;
    m_lastDelivery.invalidate();
    // Preserve the trailing dirty update when disabling the comparison mode.
    // Delivery can destroy the window and this scheduler; touch no state after it.
    if (!enabled) { deliverPending(); }
}

void WindowRepaintScheduler::setEnabledForProcess(bool enabled)
{
    s_processOverride = enabled;
    setEnabled(enabled);
}

void WindowRepaintScheduler::clearProcessOverride()
{
    s_processOverride.reset();
}

QVariantMap WindowRepaintScheduler::statsSnapshot(bool reset)
{
    const double seconds = std::max(0.001, m_statsClock.nsecsElapsed() / 1.0e9);
    QVariantMap result{
        {QStringLiteral("enabled"), m_enabled},
        {QStringLiteral("sinceSeconds"), seconds},
        {QStringLiteral("requests"), qulonglong(m_requests)},
        {QStringLiteral("deferredRequests"), qulonglong(m_deferred)},
        {QStringLiteral("updateDeliveries"), qulonglong(m_deliveries)},
        {QStringLiteral("updateDeliveriesPerSec"), m_deliveries / seconds},
        {QStringLiteral("pending"), m_pending},
        {QStringLiteral("frameIntervalMs"), frameIntervalNs() / 1.0e6}
    };
    if (reset) {
        m_statsClock.restart();
        m_requests = m_deferred = m_deliveries = 0;
    }
    return result;
}

} // namespace AetherSDR
