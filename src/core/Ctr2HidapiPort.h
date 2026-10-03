#pragma once

#include "Ctr2HidPort.h"

#include <QList>
#include <QString>

namespace AetherSDR {

// hidapi access for the CTR2 USB link: device discovery, and opening an
// interface as a Ctr2HidThreadPort. Built only with HAVE_HIDAPI.
struct Ctr2HidapiPort {
    // HID interfaces on vendor usage page 0xFF00, usage 0x01, recognised
    // CTR2 boards first.
    static QList<Ctr2HidPort::DeviceInfo> enumerate();
    // Opens the interface; nullptr and *error on failure.
    static Ctr2HidPort* open(const Ctr2HidPort::DeviceInfo& device, QString* error,
                             QObject* parent = nullptr);
};

} // namespace AetherSDR
