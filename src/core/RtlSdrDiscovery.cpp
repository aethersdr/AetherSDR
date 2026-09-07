#include "RtlSdrDiscovery.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>
#include <QSet>

#ifdef AETHER_BACKEND_RTL
#include <rtl-sdr.h>
#endif

namespace AetherSDR {

// ---------------------------------------------------------------------------
// RtlSdrDiscovery  —  pattern-matches Hl2Discovery for ConnectionPanel
// ---------------------------------------------------------------------------

RtlSdrDiscovery::RtlSdrDiscovery(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &RtlSdrDiscovery::onScanTimer);
    connect(&m_watcher, &QFutureWatcher<QVector<RadioInfo>>::finished,
            this, &RtlSdrDiscovery::onScanFinished);
}

RtlSdrDiscovery::~RtlSdrDiscovery()
{
    stop();
    m_watcher.waitForFinished();
}

bool RtlSdrDiscovery::isRunning() const noexcept
{
    return m_timer->isActive() || m_scanning;
}

bool RtlSdrDiscovery::isAvailable()
{
#ifdef AETHER_BACKEND_RTL
    return true;
#else
    return false;
#endif
}

void RtlSdrDiscovery::start(int intervalMs)
{
    if (!isAvailable()) {
        return;
    }
    m_timer->start(intervalMs);
    onScanTimer(); // immediate first scan
}

void RtlSdrDiscovery::stop()
{
    m_timer->stop();
}

// ---------------------------------------------------------------------------
// Scan logic: enumerate USB devices in a background worker thread via QtConcurrent
// ---------------------------------------------------------------------------

void RtlSdrDiscovery::onScanTimer()
{
    if (!isAvailable() || m_scanning) {
        return; // Background scan still in progress or backend disabled
    }

    m_scanning = true;
    m_watcher.setFuture(QtConcurrent::run([]() -> QVector<RadioInfo> {
        QVector<RadioInfo> current;

#ifdef AETHER_BACKEND_RTL
        QSet<QString> usedSerials;
        int count = rtlsdr_get_device_count();
        for (int i = 0; i < count; ++i) {
            char manufactBuf[256] = {0};
            char productBuf[256] = {0};
            char serialBuf[256] = {0};
            rtlsdr_get_device_usb_strings(i, manufactBuf, productBuf, serialBuf);

            RadioInfo info;
            info.family = QStringLiteral("rtl");

            QString usbSerial = QString::fromUtf8(serialBuf).trimmed();
            if (!usbSerial.isEmpty() && !usedSerials.contains(usbSerial)) {
                info.serial = usbSerial;
            } else {
                info.serial = QStringLiteral("rtl:%1").arg(i);
            }
            usedSerials.insert(info.serial);

            QString vendor = QString::fromUtf8(manufactBuf).trimmed();
            QString product = QString::fromUtf8(productBuf).trimmed();

            info.name = vendor.isEmpty()
                           ? product
                           : QStringLiteral("%1 %2").arg(vendor, product);
            if (info.name.trimmed().isEmpty()) {
                info.name = QStringLiteral("RTL-SDR");
            }

            info.model = product.isEmpty() ? QStringLiteral("RTL2832U") : product;
            info.status = QStringLiteral("Available");
            info.inUse = false;

            current.append(info);
        }
#endif
        return current;
    }));
}

void RtlSdrDiscovery::onScanFinished()
{
    m_scanning = false;
    const QVector<RadioInfo> current = m_watcher.result();

    if (!m_timer->isActive()) {
        return;
    }

    // Track which devices are still present; report lost ones
    QStringList lostSerials;
    for (auto it = m_seen.begin(); it != m_seen.end(); ++it) {
        const QString& serial = it.key();
        bool found = false;
        for (const auto& cur : current) {
            if (cur.serial == serial) {
                found = true;
                it.value().missedScans = 0;
                break;
            }
        }
        if (!found) {
            ++it.value().missedScans;
            if (it.value().missedScans >= kMissedScansBeforeLost) {
                lostSerials.append(serial);
            }
        }
    }

    // Remove lost entries
    for (const auto& serial : lostSerials) {
        emit radioLost(serial);
        m_seen.remove(serial);
    }

    // Report newly discovered or updated devices
    for (const auto& info : current) {
        if (!m_seen.contains(info.serial)) {
            m_seen[info.serial] = Seen{info, 0};
            emit radioDiscovered(info);
        } else {
            // Check if anything changed (name, model, etc.)
            if (m_seen[info.serial].info.name != info.name ||
                m_seen[info.serial].info.model != info.model) {
                m_seen[info.serial].info = info;
                emit radioUpdated(info);
            }
        }
    }
}

}  // namespace AetherSDR
