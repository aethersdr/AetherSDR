#pragma once

// Where a microHAM TRIO's configuration is kept. The connection is
// Peripherals.trio.ManualIp and Peripherals.trio.ManualPort in the Peripherals
// settings blob, beside the row's own Peripherals.trio.AutoConnect.

#include "core/PeripheralSettings.h"

#include <QString>

namespace AetherSDR {

struct MicrohamTrioSettings {
    // The Peripherals row id and the settings group.
    static constexpr const char* kPeripheralId = "trio";
    // The Trio's web interface. Its own menu can move it.
    static constexpr int kDefaultPort = 80;

    QString host;                 // empty: no TRIO configured
    int     port{kDefaultPort};

    bool isConfigured() const { return !host.isEmpty(); }

    static MicrohamTrioSettings load()
    {
        MicrohamTrioSettings out;
        const QString id = QLatin1String(kPeripheralId);
        out.host = PeripheralSettings::deviceString(id, QStringLiteral("ManualIp")).trimmed();
        const int port = PeripheralSettings::deviceInt(id, QStringLiteral("ManualPort"), kDefaultPort);
        out.port = port >= 1 && port <= 65535 ? port : kDefaultPort;
        return out;
    }

    // An unconfigured TRIO stores no address.
    void save() const
    {
        if (!isConfigured()) {
            remove();
            return;
        }
        const QString id = QLatin1String(kPeripheralId);
        PeripheralSettings::setDeviceString(id, QStringLiteral("ManualIp"), host);
        PeripheralSettings::setDeviceInt(id, QStringLiteral("ManualPort"), port);
    }

    // The address only; "Connect automatically" is the Peripherals row's to reset.
    static void remove()
    {
        PeripheralSettings::clearDeviceConnection(QLatin1String(kPeripheralId));
    }
};

} // namespace AetherSDR
