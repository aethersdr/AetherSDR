#include "core/backends/colibri/ColibriDiscovery.h"

#include "core/backends/colibri/ColibriLib.h"
#include "core/backends/colibri/ColibriSettings.h"

#include <QHostAddress>
#include <QLoggingCategory>
#include <QTimer>

Q_LOGGING_CATEGORY(lcColibriDisc, "aether.colibri.discovery")

namespace AetherSDR::colibri {

ColibriDiscovery::ColibriDiscovery(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &ColibriDiscovery::pollNow);
}

ColibriDiscovery::~ColibriDiscovery() = default;

QString ColibriDiscovery::serialForIndex(int index)
{
    return QStringLiteral("colibrinano-%1").arg(index);
}

void ColibriDiscovery::start(int intervalMs)
{
    m_timer->start(intervalMs);
    pollNow();
}

void ColibriDiscovery::stop()
{
    m_timer->stop();
}

void ColibriDiscovery::pollNow()
{
    auto& lib = ColibriLib::instance();

    // Never enumerate under a running stream — keep what we last saw. The
    // open device is, by definition, still present.
    if (lib.deviceInUse())
        return;

    if (!lib.isLoaded()) {
        QString error;
        if (!lib.ensureLoaded(ColibriSettings::dllPath(), &error)) {
            if (!m_libUnavailableLogged) {
                // Once, at info rather than warning: a PC without the vendor
                // library is the normal case for everyone without the device.
                qCInfo(lcColibriDisc) << "colibrinano_lib unavailable —"
                                      << error;
                m_libUnavailableLogged = true;
            }
            return;
        }
    }

    const int count = static_cast<int>(lib.deviceCount());

    QHash<QString, RadioInfo> now;
    for (int i = 0; i < count; ++i) {
        RadioInfo info;
        info.family = QStringLiteral("colibri");
        info.name = QStringLiteral("ColibriNANO");
        info.model = QStringLiteral("ColibriNANO");
        info.serial = serialForIndex(i);
        std::uint32_t maj = 0, min = 0, pat = 0;
        lib.version(maj, min, pat);
        info.version = QStringLiteral("%1.%2.%3").arg(maj).arg(min).arg(pat);
        info.versionLabel = QStringLiteral("lib");
        info.nickname = QStringLiteral("ColibriNANO (USB)");
        info.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
        info.port = 0;
        info.status = QStringLiteral("Available");
        info.inUse = false;
        info.multiFlexEnabled = false;
        info.isSystemModel = false;
        now.insert(info.serial, info);
    }

    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (m_seen.contains(it.key()))
            emit radioUpdated(it.value());
        else {
            qCInfo(lcColibriDisc) << "found" << it.key();
            emit radioDiscovered(it.value());
        }
    }
    for (auto it = m_seen.cbegin(); it != m_seen.cend(); ++it) {
        if (!now.contains(it.key())) {
            qCInfo(lcColibriDisc) << "lost" << it.key();
            emit radioLost(it.key());
        }
    }
    m_seen = now;
}

}  // namespace AetherSDR::colibri
