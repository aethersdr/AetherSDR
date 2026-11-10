#include "core/Ctr2RelaySettings.h"

#include "core/AppSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {
const QString kKey = QStringLiteral("Ctr2Relay");

quint16 usbId(const QJsonValue& v)
{
    const int id = v.toInt(0);
    return id > 0 && id <= 0xFFFF ? quint16(id) : 0;
}
} // namespace

Ctr2RelaySettings Ctr2RelaySettings::load()
{
    const QJsonObject o =
        QJsonDocument::fromJson(AppSettings::instance().value(kKey, QString{}).toString().toUtf8())
            .object();
    Ctr2RelaySettings s;
    s.autoStart = o.value("autoStart").toBool(false);
    s.hasSetup = o.contains("transport");
    s.usb = o.value("transport").toString() != QStringLiteral("wifi");
    for (const QJsonValue& v : o.value("usbDevices").toArray()) {
        const QJsonObject d = v.toObject();
        const UsbDevice device{d.value("serial").toString(), usbId(d.value("vendorId")),
                               usbId(d.value("productId"))};
        if (!device.serial.isEmpty() && !s.usbDevices.contains(device))
            s.usbDevices.append(device);
    }
    // Saved before several devices could relay: the one device it relayed.
    const QString serial = o.value("usbSerial").toString();
    if (!o.contains("usbDevices") && !serial.isEmpty()) {
        s.usbDevices.append(
            {serial, usbId(o.value("usbVendorId")), usbId(o.value("usbProductId"))});
    }
    s.listenAddress = o.value("listenAddress").toString();
    s.listenPort = o.value("listenPort").toString();
    return s;
}

void Ctr2RelaySettings::save() const
{
    QJsonObject o;
    o["autoStart"] = autoStart;
    if (hasSetup) {
        o["transport"] = usb ? QStringLiteral("usb") : QStringLiteral("wifi");
        QJsonArray devices;
        for (const UsbDevice& d : usbDevices) {
            devices.append(QJsonObject{{"serial", d.serial},
                                       {"vendorId", int(d.vendorId)},
                                       {"productId", int(d.productId)}});
        }
        o["usbDevices"] = devices;
        o["listenAddress"] = listenAddress;
        o["listenPort"] = listenPort;
    }
    auto& s = AppSettings::instance();
    s.setValue(kKey, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

} // namespace AetherSDR
