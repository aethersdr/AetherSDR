#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class QWidget;

namespace AetherSDR {

// QWidget keeps dirty regions until it handles UpdateRequest. Defer that
// delivery, not individual widgets or their model updates, so meters, moving
// overlays and QRhi surfaces share the same backing-store synchronization.
// Only windows containing a live pan opt in; no continuously running timer.
class WindowRepaintScheduler final : public QObject {
    Q_OBJECT
public:
    static WindowRepaintScheduler* ensureFor(QWidget* window, bool initiallyEnabled = true);
    bool isEnabled() const { return m_enabled; }
    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE QVariantMap statsSnapshot(bool reset = false);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    WindowRepaintScheduler(QWidget* window, bool initiallyEnabled);
    void deliverPending();
    qint64 frameIntervalNs() const;

    QWidget* const m_window;
    QTimer m_timer;
    QElapsedTimer m_lastDelivery;
    QElapsedTimer m_statsClock;
    bool m_enabled{true};
    bool m_delivering{false};
    bool m_pending{false};
    quint64 m_requests{0};
    quint64 m_deferred{0};
    quint64 m_deliveries{0};
};

} // namespace AetherSDR
