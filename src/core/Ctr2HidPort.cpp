#include "Ctr2HidPort.h"

namespace AetherSDR {

QString Ctr2HidPort::DeviceInfo::ctr2Model() const
{
    if (product == QLatin1String("ESP32S3_DEV")) {
        return QStringLiteral("CTR2-Max / Nano");
    }
    if (product == QLatin1String("STAMP-S3")) {
        return QStringLiteral("CTR2 (M5Dial)");
    }
    if (product == QLatin1String("XIAO_ESP32S3")) {
        return QStringLiteral("CTR2-MIDI");
    }
    return {};
}

QString Ctr2HidPort::DeviceInfo::label() const
{
    QString name = product.isEmpty() ? QStringLiteral("HID device") : product;
    if (!manufacturer.isEmpty()) {
        name = manufacturer + QLatin1Char(' ') + name;
    }
    QString id = QStringLiteral("%1:%2")
        .arg(vendorId, 4, 16, QLatin1Char('0'))
        .arg(productId, 4, 16, QLatin1Char('0'));
    if (!serial.isEmpty()) {
        id += QStringLiteral(" #") + serial;
    }
    const QString model = ctr2Model();
    const QString base = QStringLiteral("%1 (%2)").arg(name, id);
    return model.isEmpty() ? base : model + QStringLiteral(": ") + base;
}

} // namespace AetherSDR
