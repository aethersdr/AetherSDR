#pragma once

#include "Ctr2HidFraming.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

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
        // the operator still picks the device.
        QString ctr2Model() const;
        QString label() const;
    };

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

signals:
    // A whole number of 8-byte reports, in arrival order.
    void reportsReceived(const QByteArray& reports);
    void reportsSent(int count);
    // The device is gone or unusable; the port is closed.
    void failed(const QString& message);
};

} // namespace AetherSDR
