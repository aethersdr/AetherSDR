#include "core/Ctr2RelaySettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {
const QString kKey = QStringLiteral("Ctr2Relay");
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
    s.usbSerial = o.value("usbSerial").toString();
    const int vid = o.value("usbVendorId").toInt(0);
    const int pid = o.value("usbProductId").toInt(0);
    s.usbVendorId = vid > 0 && vid <= 0xFFFF ? quint16(vid) : 0;
    s.usbProductId = pid > 0 && pid <= 0xFFFF ? quint16(pid) : 0;
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
        o["usbSerial"] = usbSerial;
        o["usbVendorId"] = int(usbVendorId);
        o["usbProductId"] = int(usbProductId);
        o["listenAddress"] = listenAddress;
        o["listenPort"] = listenPort;
    }
    auto& s = AppSettings::instance();
    s.setValue(kKey, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

} // namespace AetherSDR
