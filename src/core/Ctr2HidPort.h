#pragma once

#include "Ctr2HidFraming.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>
#include <vector>

namespace AetherSDR {

// One opened CTR2 HID interface: ordered 8-byte reports in and out (report
// ID handled by the implementation). Ctr2UsbRelay drives it; tests inject a
// scripted implementation. Signals are delivered on the owner's thread.
class Ctr2HidPort : public QObject {
    Q_OBJECT

public:
    struct DeviceInfo {
        QString path;
        quint16 vendorId{0};
        quint16 productId{0};
        QString manufacturer;
        QString product;
        QString serial;

        // The CTR2 model these USB IDs belong to, or empty. They are the
        // ESP32-S3 boards' own IDs, so a match is a strong hint, not proof;
        // the first match is preselected and the operator may pick another.
        QString ctr2Model() const;
        // For the device list: the model, the maker and product, and the
        // USB IDs and serial. A device that names itself (its product is its
        // model) is named once.
        QString label() const;
        // Just the name: the model, else the product.
        QString shortName() const;
        // A recognized device whose firmware relays over USB: AetherKnob,
        // firmware that names itself (AetherCTR2), or a CTR2-Max / Nano on
        // Lynovation's firmware (v1.02.00 on; earlier versions never answer).
        bool usbLink() const;
    };

    // Every USB identity recognised as a CTR2. 303A:1001 is Espressif's
    // default for any ESP32-S3, so the product string decides. A product
    // ending in '*' is a prefix (a udev glob): a device that matches it names
    // itself, and its product string is shown as its model, once it passes
    // selfNamedModel()'s checks; such an entry's model is nullptr.
    // packaging/linux/70-aethersdr-ctr2.rules must list exactly these
    // product patterns. udev's '*' matches any suffix, so for a '*' entry the
    // rule grants access to a superset that selfNamedModel() then narrows.
    struct KnownCtr2 {
        quint16 vendorId;
        quint16 productId;
        const char* product;
        const char* model;
        // Its firmware relays over USB (the link this relay speaks).
        bool usbLink{false};
    };
    static const std::vector<KnownCtr2>& knownCtr2Devices();

    using QObject::QObject;
    ~Ctr2HidPort() override = default;

    virtual bool isOpen() const = 0;
    // Queues reports for transmission in order; reportsSent() reports progress.
    virtual void send(const std::vector<ctr2hid::Report>& reports) = 0;
    // Fence: drops every report passed to send() before this call that has
    // not been written yet. reportsSent() afterwards counts only reports
    // sent after the fence, never late acknowledgements from before it.
    virtual void discardQueued() = 0;
    // Drops anything queued, writes finalReports, closes the device and
    // deletes this port once that has finished. Never blocks the caller,
    // which must not use the port afterwards.
    virtual void shutdown(const std::vector<ctr2hid::Report>& finalReports) = 0;
    virtual QString description() const = 0;

    // Link extensions negotiated with the device (ctr2hid::capabilityBit
    // mask); 0 for plain link version 0, and until negotiation finishes.
    quint32 extensions() const { return m_extensions; }
    // The extensions the device offered (Feature report 0x02), whether or
    // not the host took them: what it could do on another session.
    quint32 offeredExtensions() const { return m_offered; }
    // The device's firmware version (Feature report 0x03), read during
    // negotiation; empty for a device without it, and until then.
    const std::optional<ctr2hid::deviceinfo::Info>& firmwareInfo() const { return m_firmwareInfo; }

signals:
    // A whole number of 8-byte reports, in arrival order.
    void reportsReceived(const QByteArray& reports);
    void reportsSent(int count);
    // The device is gone or unusable; the port is closed.
    void failed(const QString& message);
    // Negotiation finished; delivered before any report arrives.
    void extensionsNegotiated(quint32 extensions);

protected:
    // The firmware version first: extensionsNegotiated() announces both.
    void setExtensions(quint32 extensions,
                       std::optional<ctr2hid::deviceinfo::Info> info = std::nullopt,
                       quint32 offered = 0)
    {
        m_firmwareInfo = std::move(info);
        m_extensions = extensions;
        m_offered = offered;
        emit extensionsNegotiated(extensions);
    }

private:
    quint32 m_extensions{0};
    quint32 m_offered{0};
    std::optional<ctr2hid::deviceinfo::Info> m_firmwareInfo;
};

} // namespace AetherSDR
