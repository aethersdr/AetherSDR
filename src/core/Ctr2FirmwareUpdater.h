#pragma once

#include "Ctr2HidFraming.h"
#include "Ctr2HidPort.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

class QTimer;

namespace AetherSDR {

// Replaces a CTR2-class device's application over its USB link, through the
// Firmware link extension (docs/ctr2-usb-relay-design.md, "Firmware"). The
// device writes the image to its spare slot, checks its hash and signature,
// and restarts into it; the new image keeps itself only once it completes a
// link, which the updater does before reporting success. A device that
// resets first, or rejects the image, keeps the firmware it had.
//
// The relay must not hold the device meanwhile: only one program may drive
// the link. Ctr2ProxyModel stops it first.
class Ctr2FirmwareUpdater : public QObject {
    Q_OBJECT

public:
    // Opens a device with the Firmware extension accepted.
    using PortOpener =
        std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo& device, QString* error)>;
    using DeviceSource = std::function<QList<Ctr2HidPort::DeviceInfo>()>;

    // An application image for an ESP32-S3 device, as read from a file.
    struct Image {
        QByteArray bytes;
        QString project;   // esp_app_desc project_name
        QString version;   // esp_app_desc version
        // version as the device reports it: "major.minor.patch" from the
        // front of version, each 0-255.
        QString reportedVersion;
    };
    // Checks that file is an ESP32-S3 application image (not a merged flash
    // image or a bootloader) with an app description, small enough for one
    // app slot. On failure returns false and a reason the operator can act on.
    static bool readImage(const QByteArray& file, Image* image, QString* problem);
    static constexpr int kMaxImageBytes = 0x300000;

    // Timing, adjustable by tests.
    struct Timing {
        int helloRetryMs{1000};
        int answerTimeoutMs{5000};   // READY, and each Status
        int endTimeoutMs{30000};     // End: the device verifies the whole image
        int returnTimeoutMs{30000};  // the device leaving and coming back
        int firstLookMs{1500};       // after End: let the device leave the bus first
        int pollMs{500};             // looking for the device to come back
    };

    Ctr2FirmwareUpdater(DeviceSource devices, PortOpener opener, QObject* parent = nullptr);
    ~Ctr2FirmwareUpdater() override;

    void setTiming(const Timing& timing) { m_timing = timing; }
    // Chunks sent before waiting for their Status (1 = stop-and-wait).
    void setWindow(int chunks) { m_window = qMax(1, chunks); }

    // Starts updating device with image (from readImage). False if an update
    // is already running.
    bool start(const Ctr2HidPort::DeviceInfo& device, const Image& image);
    // Instead of an image: asks a device whose firmware takes updates over
    // the link to restart into its other firmware (the maker's, beside it) or
    // into its ROM download mode. finished() follows once the device has
    // agreed and is restarting. False if an update is already running.
    enum class Restart { OtherFirmware, Bootloader };
    bool restartDevice(const Ctr2HidPort::DeviceInfo& device, Restart target);
    // Abandons the update; the device keeps its current firmware unless End
    // was already answered. finished() follows.
    void cancel();
    bool isRunning() const { return m_phase != Phase::Idle; }
    // The last restart request was written but never answered: the device
    // may still be leaving the bus, though finished() reported a failure.
    bool restartUnanswered() const { return m_restartUnanswered; }

signals:
    void progress(qint64 bytesStored, qint64 imageBytes);
    // A short description of the current step, for display.
    void stepChanged(const QString& step);
    // ok: the device runs the new image and has kept it; version is what it
    // reports. Otherwise message says why, and whether the old firmware stays.
    void finished(bool ok, const QString& message, const QString& version);

private:
    enum class Phase { Idle, Calling, Begin, Sending, Ending, Returning, Confirming, Verifying,
                       Commanding };

    bool beginSession(const Ctr2HidPort::DeviceInfo& device, const Image& image,
                      std::optional<Restart> restart);
    void setPhase(Phase phase, const QString& step);
    bool openPort(QString* error);
    void closePort(const std::vector<ctr2hid::Report>& finalReports = {});
    // ABORT and CLOSED for a session in progress, CLOSED while calling.
    std::vector<ctr2hid::Report> sessionEndReports();
    void onExtensions(quint32 extensions);
    void onReports(const QByteArray& reports);
    void onMessage(const ctr2hid::Message& message);
    void onStatus(const ctr2hid::firmware::Reply& reply);
    void onPortFailed(const QString& message);
    void onTimeout();
    void sendHello();
    // False when nothing could be queued (no port, or the payload would not encode).
    bool sendExtension(const QByteArray& payload);
    void sendChunks();
    void lookForDevice();
    void fail(const QString& message);
    void succeed(const QString& version);
    // A restart request done: the device is leaving the bus for its target.
    void restarted();
    void armTimeout(int ms);

    DeviceSource m_devices;
    PortOpener m_opener;
    Timing m_timing;
    int m_window{4};

    Phase m_phase{Phase::Idle};
    Ctr2HidPort::DeviceInfo m_device;
    Image m_image;
    std::optional<Restart> m_restart;   // a restart request instead of an image
    QByteArray m_sha256;
    QPointer<Ctr2HidPort> m_port;
    ctr2hid::FrameEncoder m_encoder;
    ctr2hid::FrameReassembler m_reassembler;
    QTimer* m_timeout{nullptr};
    QTimer* m_retry{nullptr};
    qint64 m_sent{0};       // image bytes sent
    qint64 m_stored{0};     // image bytes the device has acknowledged
    int m_inFlight{0};
    // Reports this port has queued and written, and the queued count once the
    // restart request was queued: it has reached the device's endpoint when
    // the written count gets there.
    qint64 m_reportsQueued{0};
    qint64 m_reportsWritten{0};
    qint64 m_restartQueuedTo{0};
    bool m_restartUnanswered{false};
    QString m_previousVersion;
};

} // namespace AetherSDR
