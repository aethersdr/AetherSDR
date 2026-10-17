#pragma once

#include "core/Ctr2FirmwareUpdater.h"
#include "core/Ctr2HidPort.h"
#include "core/Ctr2MaxStockCatalog.h"
#include "core/Ctr2MaxStockFirmware.h"
#include "core/EspFirmwareCatalog.h"
#include "core/EspFirmwareJob.h"
#include "core/EspFirmwareManifest.h"

#include <QByteArray>
#include <QDateTime>
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
        QString hidPath;   // running firmware: its USB HID device
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
    Target target() const { return {m_device, m_port, m_mac, m_hidPath}; }
    // A unit whose firmware takes the link's FIRMWARE commands
    // (Ctr2ProxyModel::usbFirmwareCommands()): AetherCTR2, or stock firmware
    // that offers them. It restarts into its other firmware or download mode
    // on request, with no buttons. It must be the relay's selected device.
    bool takesCommands(const Target& target) const;
    // Looks for the device again now, rather than at the next poll.
    void lookAgain() { poll(); }
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

    // The CTR2-Max's stock firmware from a file the operator downloaded from
    // lynovation.com (Ctr2MaxStockFirmware); AetherSDR never fetches it.
    // Each one opened is kept in firmwareFolder(), one per version, so an
    // earlier version can be installed again later. False, with *problem, if
    // the file is not that firmware or cannot be kept; then nothing changes.
    bool openStockFirmware(const QString& path, QString* problem);
    struct StoredFirmware {
        QString path;
        QString name;      // "CTR2-Max firmware v1.02.00n"
        QString version;   // "v1.02.00n"; empty if the image names none
    };
    // The firmware kept, newest version first.
    QList<StoredFirmware> storedFirmware() const { return m_stored; }
    // Chooses one of them for installStockFirmware().
    bool chooseStockFirmware(const QString& path, QString* problem);

    // Lynovation's own list of its releases (Ctr2MaxStockCatalog), fetched
    // from lynovation.com when the window opens; empty until it arrives, or
    // when Lynovation publishes none (or none signed with a trusted key).
    QList<Ctr2MaxStockCatalog::Release> stockReleases() const { return m_stockReleases.releases; }
    // Downloads one of them from lynovation.com, keeps it as a file opened
    // here would be, and chooses it: stockDownloaded(), or stockDownloadFailed().
    void downloadStockRelease(int index);
    bool downloadingStockRelease() const { return m_lynDownloader->isBusy(); }
    bool haveStockFirmware() const { return m_haveStock; }
    QString stockFirmwarePath() const { return m_stockPath; }   // the one chosen
    // Its version as the firmware shows it, else the file's name.
    QString stockFirmwareName() const;
    static QString firmwareFolder();
    // Tests: a folder of their own.
    static void setFirmwareFolderForTesting(const QString& folder);
    // The actions below run on target in download mode: at once if it is
    // there, else once it gets there, restarted into it over the link from
    // AetherCTR2, or by its buttons from its stock firmware (the window tells
    // the operator how). Waiting counts as busy, and cancel() ends it.
    //
    // Installs: the whole flash is backed up first, to backupFolder(),
    // keeping the latest kKeptBackups per device.
    // The chosen AetherCTR2 release (in download mode; an update over the
    // link is installOverLink()).
    bool installRelease(const Target& target, QString* problem);
    // The opened file's firmware, the CTR2-Max's own.
    bool installStockFirmware(const Target& target, QString* problem);
    // Restarts the device into what is installed on it, without writing
    // anything but the boot selection. From AetherCTR2: over the link. To a
    // firmware from download mode, or from the stock firmware (once in
    // download mode): the boot selection, laid out by the unit's own table.
    // Bootloader from the stock firmware takes the unit's buttons, which the
    // window asks for: refused here.
    enum class Firmware { AetherCtr2, Stock, Bootloader };
    bool restartInto(const Target& target, Firmware firmware, QString* problem);
    // Puts a backup back: the whole flash as it was, on the device it came
    // from only.
    bool restoreBackup(const Target& target, const QString& path, QString* problem);
    bool waitingForDownloadMode() const { return m_waiting; }

    // The connected device's backups, newest first: every .bin with its
    // description (.json) beside it, in backupFolder() or the folder above
    // it, where earlier versions suggested saving them; of every device when
    // its MAC is not known (its stock firmware does not report it). A restore
    // checks the file and the MAC itself.
    struct Backup {
        QString path;
        QString mac;      // "AA:BB:CC:DD:EE:FF"
        QDateTime made;
    };
    QList<Backup> backups() const;
    static QString backupFolder();
    // Tests: a folder of their own, never the operator's Documents.
    static void setBackupFolderForTesting(const QString& folder);
    static constexpr int kKeptBackups = 3;
    QString lastBackupPath() const { return m_lastBackup; }

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
    void stockDownloaded(const QString& path);
    void stockDownloadFailed(const QString& message);

private:
    void poll();
    bool working() const;   // a job or a link action runs
    using Start = std::function<bool(const Target& target, QString* problem)>;
    bool whenInDownloadMode(const Target& target, Start start, QString* problem);
    void startWhenInDownloadMode();
    // An install, after a backup: the release, or plan's firmware.
    bool startInstall(const Target& target, const EspFirmwareJob::Request::Plan& plan,
                      QString* problem);
    void pruneBackups(const QString& mac);
    bool stillTarget(const Target& target, QString* problem);
    bool runJob(const Target& target, EspFirmwareJob::Action action, const QString& backupPath,
                const QString& restoreFrom, QString* problem,
                const EspFirmwareJob::Request::Plan& plan = {});
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
    QString m_hidPath;
    int m_loaderCount{0};
    bool m_linkRunning{false};   // an over-the-link update or restart started here

    void loadStoredFirmware();
    // Keeps firmware in firmwareFolder() as its version's .bin; *kept is its path.
    bool keepFirmware(const Ctr2MaxStockFirmware& firmware, const QString& fallbackName,
                      QString* kept, QString* problem);
    Ctr2MaxStockDownloader* m_lynDownloader{nullptr};
    Ctr2MaxStockCatalog m_stockReleases;
    bool m_stockDownloadAsked{false};
    struct KeptFile {   // a .bin in firmwareFolder(), as last read
        qint64 size{-1};
        QDateTime modified;
        bool valid{false};
        StoredFirmware firmware;
    };
    QHash<QString, KeptFile> m_keptFiles;
    Ctr2MaxStockFirmware m_stock;
    QString m_stockPath;          // the kept file it came from
    bool m_haveStock{false};
    QList<StoredFirmware> m_stored;
    bool m_waiting{false};        // for the device to be in download mode
    Start m_waitStart;            // then this
    QString m_waitMac;            // the device waited for; empty: the only one
    int m_waitSeen{0};            // polls in a row it has been found in download mode
    QString m_lastBackup;         // the install's backup, while it runs and after
    bool m_installRunning{false}; // an install made here, with its backup
};

} // namespace AetherSDR
