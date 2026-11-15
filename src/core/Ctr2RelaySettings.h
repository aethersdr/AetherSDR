#pragma once

// CTR2 relay persistence: one JSON object under AppSettings "Ctr2Relay".
// Whether the relay starts by itself, and the setup it last started with,
// which a later automatic start uses. Nothing is saved for a relay that has
// never been started.

#include <QList>
#include <QString>

namespace AetherSDR {

struct Ctr2RelaySettings {
    // A USB device relayed before, matched by serial number and IDs: its
    // device path can change from one boot or port to the next. Its name, as
    // listed when it was last started, is for showing it while it is
    // unplugged only, and is not part of the match; settings from before
    // have none.
    struct UsbDevice {
        QString serial;
        quint16 vendorId{0};
        quint16 productId{0};
        QString name{};
        bool operator==(const UsbDevice& o) const
        {
            return serial == o.serial && vendorId == o.vendorId && productId == o.productId;
        }
    };

    bool autoStart{false};
    bool usb{true};               // false: Wi-Fi (TCP)
    // Every USB device relayed so far, in the order first started; an
    // automatic start starts each one that is plugged in. Settings from
    // before several devices could relay at once hold one.
    QList<UsbDevice> usbDevices;
    // The operator forgot the last remembered device (Forget): an automatic
    // start then starts none, not even the selected one, until a device is
    // started by hand. Without it, an empty list starts the selected device.
    bool usbForgotten{false};
    QString listenAddress;        // Wi-Fi
    QString listenPort;           // Wi-Fi; empty for the default
    bool hasSetup{false};         // a setup was saved (the relay has started)

    static Ctr2RelaySettings load();
    void save() const;
};

} // namespace AetherSDR
