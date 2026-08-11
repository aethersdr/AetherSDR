#include "core/backends/ft991/Ft991Discovery.h"

#include <QHostAddress>
#include <QLoggingCategory>
#include <QSerialPortInfo>
#include <QTimer>

Q_LOGGING_CATEGORY(lcFt991Disc, "aether.ft991.discovery")

namespace AetherSDR::ft991 {

namespace {

// The FT-991's built-in USB bridge is a Silicon Labs CP2105 (dual UART).
// Match on what QSerialPortInfo actually reports for it; the "Standard COM
// Port" half is the radio's GPS/firmware channel, not CAT.
bool looksLikeFt991Port(const QSerialPortInfo& info)
{
    const QString desc = info.description().toLower();
    const QString manu = info.manufacturer().toLower();
    const bool cp210 = desc.contains(QLatin1String("cp210"))
        || manu.contains(QLatin1String("silicon lab"));
    if (!cp210)
        return false;
    if (desc.contains(QLatin1String("standard com port")))
        return false;
    return true;
}

}  // namespace

Ft991Discovery::Ft991Discovery(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &Ft991Discovery::pollNow);
}

Ft991Discovery::~Ft991Discovery() = default;

QString Ft991Discovery::serialForPort(const QString& portName)
{
    return QStringLiteral("ft991-%1").arg(portName);
}

void Ft991Discovery::start(int intervalMs)
{
    m_timer->start(intervalMs);
    pollNow();
}

void Ft991Discovery::stop()
{
    m_timer->stop();
}

void Ft991Discovery::pollNow()
{
    QHash<QString, RadioInfo> now;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& port : ports) {
        if (!looksLikeFt991Port(port))
            continue;
        RadioInfo info;
        info.family = QStringLiteral("ft991");
        info.name = QStringLiteral("FT-991");
        info.model = QStringLiteral("FT-991");
        info.serial = serialForPort(port.portName());
        info.nickname = QStringLiteral("FT-991 (%1)").arg(port.portName());
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
            qCInfo(lcFt991Disc) << "candidate" << it.key();
            emit radioDiscovered(it.value());
        }
    }
    for (auto it = m_seen.cbegin(); it != m_seen.cend(); ++it) {
        if (!now.contains(it.key())) {
            qCInfo(lcFt991Disc) << "lost" << it.key();
            emit radioLost(it.key());
        }
    }
    m_seen = now;
}

}  // namespace AetherSDR::ft991
