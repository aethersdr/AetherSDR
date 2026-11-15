#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace AetherSDR {

/**
 * @brief Discovers RTL-SDR USB dongles via librtlsdr on a background worker thread.
 *
 * Emits RadioInfo with family="rtl" so ConnectionPanel's existing
 * onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it unchanged.
 * Pattern-matches Hl2Discovery for consistent multi-backend discovery.
 */
class RtlSdrDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit RtlSdrDiscovery(QObject* parent = nullptr);
    ~RtlSdrDiscovery() override;

    // Start/stop periodic USB scans
    void start(int intervalMs = 5000);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept;

    // True when this build includes librtlsdr support.
    static bool isAvailable();

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private slots:
    void onScanTimer();
    void onScanFinished();

private:
    struct Seen {
        RadioInfo info;
        int missedScans = 0;
    };

    QTimer* m_timer = nullptr;
    QFutureWatcher<QVector<RadioInfo>> m_watcher;
    bool m_scanning = false;
    QHash<QString, Seen> m_seen;  // keyed by serial (USB serial or index)
    static constexpr int kMissedScansBeforeLost = 3;
};

}  // namespace AetherSDR
