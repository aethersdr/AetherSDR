#include "Ctr2HidPort.h"

namespace AetherSDR {

const std::vector<Ctr2HidPort::KnownCtr2>& Ctr2HidPort::knownCtr2Devices()
{
    // VID:PID and product string as the CTR2 firmware reports them.
    static const std::vector<KnownCtr2> kDevices{
        // Lynovation's firmware relays over USB from v1.02.00 on; an earlier
        // version never answers the link, and the relay says so.
        {0x303A, 0x1001, "ESP32S3_DEV", "CTR2-Max / Nano", true},
        {0x303A, 0x1001, "M5STACK_DIAL", "CTR2 (M5Dial)"},
        {0x303A, 0x1001, "STAMP-S3", "CTR2 (M5Dial)"},
        {0x2886, 0x0056, "XIAO_ESP32S3", "CTR2-MIDI"},
        // Elecrow CrowPanel 2.1" rotary display running AetherKnob firmware;
        // it speaks the same link and radio protocol as a CTR2.
        {0x303A, 0x1001, "AETHER_KNOB", "AetherKnob", true},
        // Other controller firmware that speaks the same link: it names
        // itself with a product string starting "Aether".
        {0x303A, 0x1001, "Aether*", nullptr, true},
    };
    return kDevices;
}

namespace {

// A self-named device's product string is untrusted input that becomes its
// model name: accept it only as a short, printable ASCII name that extends
// the prefix.
QString selfNamedModel(const QString& product, QLatin1String prefix)
{
    constexpr qsizetype kMaxName = 32;
    if (!product.startsWith(prefix) || product.size() <= prefix.size()
        || product.size() > kMaxName) {
        return {};
    }
    for (const QChar c : product) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7E) {
            return {};
        }
    }
    return product;
}

} // namespace

// The known-device entry a device matches, the first in the table's order,
// with its model name; nullptr if none.
static const Ctr2HidPort::KnownCtr2* matchKnown(const Ctr2HidPort::DeviceInfo& d, QString* model)
{
    for (const Ctr2HidPort::KnownCtr2& known : Ctr2HidPort::knownCtr2Devices()) {
        if (d.vendorId != known.vendorId || d.productId != known.productId)
            continue;
        const QLatin1String pattern(known.product);
        if (pattern.endsWith(QLatin1Char('*'))) {
            const QString name = selfNamedModel(d.product, pattern.chopped(1));
            if (!name.isEmpty()) {
                *model = name;
                return &known;
            }
        } else if (d.product == pattern) {
            *model = QString::fromLatin1(known.model);
            return &known;
        }
    }
    return nullptr;
}

bool Ctr2HidPort::DeviceInfo::usbLink() const
{
    QString model;
    const KnownCtr2* known = matchKnown(*this, &model);
    return known && known->usbLink;
}

QString Ctr2HidPort::DeviceInfo::ctr2Model() const
{
    QString model;
    return matchKnown(*this, &model) ? model : QString();
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
    if (!model.isEmpty() && model == product)
        return QStringLiteral("%1 (%2)").arg(model, id);
    const QString base = QStringLiteral("%1 (%2)").arg(name, id);
    return model.isEmpty() ? base : model + QStringLiteral(": ") + base;
}

QString Ctr2HidPort::DeviceInfo::shortName() const
{
    const QString model = ctr2Model();
    if (!model.isEmpty())
        return model;
    return product.isEmpty() ? QStringLiteral("HID device") : product;
}

} // namespace AetherSDR
