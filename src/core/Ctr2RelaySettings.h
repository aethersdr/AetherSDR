#pragma once

// CTR2 relay persistence: one JSON object under AppSettings "Ctr2Relay".
// Whether the relay starts by itself, and the setup it last started with,
// which a later automatic start uses. Nothing is saved for a relay that has
// never been started.

#include <QString>

namespace AetherSDR {

struct Ctr2RelaySettings {
    bool autoStart{false};
    bool usb{true};               // false: Wi-Fi (TCP)
    // The USB device it last relayed, matched by serial number and IDs: its
    // device path can change from one boot or port to the next.
    QString usbSerial;
    quint16 usbVendorId{0};
    quint16 usbProductId{0};
    QString listenAddress;        // Wi-Fi
    QString listenPort;           // Wi-Fi; empty for the default
    bool hasSetup{false};         // a setup was saved (the relay has started)

    static Ctr2RelaySettings load();
    void save() const;
};

} // namespace AetherSDR
