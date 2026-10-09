#include "Ctr2HidPort.h"

namespace AetherSDR {

const std::vector<Ctr2HidPort::KnownCtr2>& Ctr2HidPort::knownCtr2Devices()
{
    // VID:PID and product string as the CTR2 firmware reports them.
    static const std::vector<KnownCtr2> kDevices{
        {0x303A, 0x1001, "ESP32S3_DEV", "CTR2-Max / Nano"},
        {0x303A, 0x1001, "M5STACK_DIAL", "CTR2 (M5Dial)"},
        {0x303A, 0x1001, "STAMP-S3", "CTR2 (M5Dial)"},
        {0x2886, 0x0056, "XIAO_ESP32S3", "CTR2-MIDI"},
        // Elecrow CrowPanel 2.1" rotary display running AetherKnob firmware;
        // it speaks the same link and radio protocol as a CTR2.
        {0x303A, 0x1001, "AETHER_KNOB", "AetherKnob"},
    };
    return kDevices;
}

QString Ctr2HidPort::DeviceInfo::ctr2Model() const
{
    for (const KnownCtr2& known : knownCtr2Devices()) {
        if (vendorId == known.vendorId && productId == known.productId
            && product == QLatin1String(known.product)) {
            return QString::fromLatin1(known.model);
        }
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
