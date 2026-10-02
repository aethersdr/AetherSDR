#include "Ctr2HidPort.h"

namespace AetherSDR {

QString Ctr2HidPort::DeviceInfo::ctr2Model() const
{
    // VID:PID and product string as the CTR2 firmware reports them. 303A:1001
    // is Espressif's default for any ESP32-S3, so the product string decides.
    constexpr quint16 kEspressif = 0x303A;
    constexpr quint16 kEspressifS3 = 0x1001;
    constexpr quint16 kSeeed = 0x2886;
    constexpr quint16 kSeeedXiaoS3 = 0x0056;
    if (vendorId == kEspressif && productId == kEspressifS3) {
        if (product == QLatin1String("ESP32S3_DEV")) {
            return QStringLiteral("CTR2-Max / Nano");
        }
        if (product == QLatin1String("M5STACK_DIAL") || product == QLatin1String("STAMP-S3")) {
            return QStringLiteral("CTR2 (M5Dial)");
        }
    }
    if (vendorId == kSeeed && productId == kSeeedXiaoS3
        && product == QLatin1String("XIAO_ESP32S3")) {
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
