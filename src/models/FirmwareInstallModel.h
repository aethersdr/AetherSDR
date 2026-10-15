#pragma once

#include "core/Ctr2FirmwareUpdater.h"
#include "core/Ctr2HidPort.h"
#include "core/EspFirmwareCatalog.h"
#include "core/EspFirmwareJob.h"
#include "core/EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace AetherSDR {

class Ctr2ProxyModel;

// The CTR2-Max firmware installer behind the CTR2 window: the release list
// (EspFirmwareCatalog), which CTR2-Max is connected and in what mode, and
// the actions on it, run by EspFirmwareJob in download mode or by the CTR2
// relay's updater (Ctr2ProxyModel) over the link. AetherCTR2 is installed
// only on registered units (Ctr2MaxRegistration), as agreed with Lynovation.
//
// An action is taken on the device the operator confirmed: the window
// captures target() when the action begins, and the model refuses it if the
// device has changed since, and the job checks the MAC again once it has
// opened the port, before anything is written.
class FirmwareInstallModel : public QObject {
    Q_OBJECT

public:
    enum class Device {
        None,          // nothing found
        Bootloader,    // in download mode
        LinkFirmware,  // runs firmware that takes updates over the link (AetherCTR2)
        Stock,         // runs its stock firmware
    };
    // A device in download mode, as found.
    struct Loader {
        QString port;
        QString mac;   // "AA:BB:CC:DD:EE:FF"
    };
    // The device an action is for: captured when the action begins.
    struct Target {
        Device device{Device::None};
        QString port;  // download mode
        QString mac;
    };

    explicit FirmwareInstallModel(Ctr2ProxyModel* relay, QObject* parent = nullptr);
    ~FirmwareInstallModel() override;

    // Fetches the release list and starts watching for the device.
    void begin();

    // Tests: the devices found, and the device a job opens.
    using LoaderSource = std::function<QList<Loader>()>;
    // Controllers running firmware: by default the relay's own device list
    // (it rescans while stopped), so there is one enumeration and one
    // selected device.
    using HidSource = std::function<QList<Ctr2HidPort::DeviceInfo>()>;
    void setDeviceSources(LoaderSource loaders, HidSource hid);
    void setJobTargetOpener(EspFirmwareJob::TargetOpener opener);
    void setReleaseForTesting(const EspFirmwareManifest& manifest,
                              const QHash<QString, QByteArray>& files);

    // Releases, as listed ("AetherCTR2 0.9.2 for CTR2-Max"); empty until the
    // list arrives, and catalogProblem() says why it has not.
    QStringList releases() const { return m_releases; }
    QString catalogProblem() const { return m_catalogProblem; }
    void chooseRelease(int index);
    // What the release step says now: its description, the download, the
    // release ready, or why not.
    QString releaseText() const { return m_releaseText; }
    bool haveRelease() const { return m_haveRelease; }
    QString releaseProduct() const { return m_manifest.product; }
    QString releaseVersion() const { return m_manifest.version; }

    Device device() const { return m_device; }
    QString deviceName() const { return m_deviceName; }   // LinkFirmware: its name
    QString deviceMac() const { return m_mac; }
    int loaderCount() const { return m_loaderCount; }
    QString linkVersion() const;   // LinkFirmware: the version it runs
    Target target() const { return {m_device, m_port, m_mac}; }
    bool busy() const;

    // Download mode. False, with *problem, if it cannot start, including when
    // the device is no longer target.
    bool install(const Target& target, const QString& backupPath, QString* problem);
    bool startFirmware(const Target& target, QString* problem);
    bool backup(const Target& target, const QString& path, QString* problem);
    bool restore(const Target& target, const QString& backupPath, QString* problem);

    // Over the link, on a LinkFirmware device: whether this release can be
    // installed that way, then installing it, or restarting the device into
    // its stock firmware or download mode.
    bool canInstallOverLink(QString* problem) const;
    bool installOverLink(const Target& target, QString* problem);
    bool restart(const Target& target, bool bootloader, QString* problem);

    void cancel();

signals:
    void releasesChanged();
    void releaseTextChanged();
    void deviceChanged();
    void busyChanged();
    void connected(const QString& chip, const QByteArray& mac, quint32 flashSize);
    void progress(const QString& step, qint64 done, qint64 total);
    void stepChanged(const QString& step);
    // ok with a message: done, but with something to know (the device did
    // not restart by itself).
    void finished(bool ok, const QString& message);

private:
    void poll();
    bool stillTarget(const Target& target, QString* problem);
    bool runJob(const Target& target, EspFirmwareJob::Action action, const QString& backupPath,
                const QString& restoreFrom, QString* problem);
    QByteArray appImage() const;
    bool linkImage(Ctr2FirmwareUpdater::Image* image, QString* problem) const;
    // The relay's selected USB device is target (a link action goes to it).
    bool relayHolds(const Target& target, QString* problem) const;
    void onJobFinished(bool ok, const QString& message);

    QPointer<Ctr2ProxyModel> m_relay;
    EspFirmwareDownloader* m_downloader{nullptr};
    EspFirmwareJob* m_job{nullptr};
    QTimer* m_poll{nullptr};
    LoaderSource m_loaders;
    HidSource m_hid;

    EspFirmwareCatalog m_catalog;
    QStringList m_releases;
    QString m_catalogProblem;
    QString m_releaseText;
    bool m_haveRelease{false};
    EspFirmwareManifest m_manifest;
    QHash<QString, QByteArray> m_files;

    Device m_device{Device::None};
    QString m_deviceName;
    QString m_port;
    QString m_mac;
    int m_loaderCount{0};
    bool m_linkRunning{false};   // an over-the-link update or restart started here
};

} // namespace AetherSDR
