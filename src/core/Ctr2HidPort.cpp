#include "Ctr2HidPort.h"

namespace AetherSDR {

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
    return QStringLiteral("%1 (%2)").arg(name, id);
}

} // namespace AetherSDR
