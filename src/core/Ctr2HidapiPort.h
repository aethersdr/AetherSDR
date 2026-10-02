#pragma once

#include "Ctr2HidPort.h"

class QThread;

namespace AetherSDR {

namespace detail {
class Ctr2HidIoWorker;
}

// hidapi-backed Ctr2HidPort. hidapi reads and writes block, so all device
// I/O runs on one dedicated thread; results come back to the owner's thread
// as signals. Built only with HAVE_HIDAPI.
class Ctr2HidapiPort final : public Ctr2HidPort {
    Q_OBJECT

public:
    // HID interfaces on vendor usage page 0xFF00, usage 0x01.
    static QList<DeviceInfo> enumerate();
    // Opens the interface at `path`; nullptr and *error on failure.
    static Ctr2HidapiPort* open(const DeviceInfo& device, QString* error,
                                QObject* parent = nullptr);

    ~Ctr2HidapiPort() override;

    bool isOpen() const override { return m_open; }
    void send(const std::vector<ctr2hid::Report>& reports) override;
    void close() override;
    void closeAfter(const std::vector<ctr2hid::Report>& finalReports) override;
    QString description() const override { return m_description; }

private:
    friend class detail::Ctr2HidIoWorker;
    Ctr2HidapiPort(void* device, const QString& description, QObject* parent);

    void deliverReceived(const QByteArray& reports);
    void deliverSent(int count);
    void deliverFailure(const QString& message);

    QThread* m_thread{nullptr};
    detail::Ctr2HidIoWorker* m_worker{nullptr};
    QString m_description;
    bool m_open{false};
};

} // namespace AetherSDR
