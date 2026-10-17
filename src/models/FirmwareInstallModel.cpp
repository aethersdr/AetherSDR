#include "models/FirmwareInstallModel.h"

#include "core/Ctr2FirmwareUpdater.h"
#include "models/Ctr2ProxyModel.h"
#ifdef HAVE_SERIALPORT
#include "core/EspFlasher.h"
#endif
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR {

namespace {

constexpr int kPollMs = 1000;
// The USB product string of the CTR2-Max's stock firmware.
constexpr char kStockProduct[] = "ESP32S3_DEV";
// Lynovation's download (a zip with manuals) is a few MB.
constexpr qint64 kMaxFirmwareFile = 64 * 1024 * 1024;

} // namespace

FirmwareInstallModel::FirmwareInstallModel(Ctr2ProxyModel* relay, QObject* parent)
    : QObject(parent)
    , m_relay(relay)
    , m_downloader(new EspFirmwareDownloader(this))
    , m_job(new EspFirmwareJob(this))
    , m_poll(new QTimer(this))
    , m_lynDownloader(new Ctr2MaxStockDownloader(this))
{
    connect(m_lynDownloader, &Ctr2MaxStockDownloader::catalogReady, this,
            [this](const Ctr2MaxStockCatalog& catalog) {
                m_stockReleases = catalog;
                emit releasesChanged();
            });
    connect(m_lynDownloader, &Ctr2MaxStockDownloader::releaseReady, this,
            [this](const Ctr2MaxStockCatalog::Release& release, const QByteArray& image) {
                Ctr2MaxStockFirmware firmware;
                QString problem, kept;
                if (!Ctr2MaxStockFirmware::read(image, &firmware, &problem)) {
                    emit stockDownloadFailed(problem);
                    return;
                }
                // The firmware is the version the signed list names.
                if (firmware.version() != release.version) {
                    emit stockDownloadFailed(
                        tr("Lynovation's list names %1, but the file it points to is %2.")
                            .arg(release.version,
                                 firmware.version().isEmpty() ? tr("unnamed")
                                                              : firmware.version()));
                    return;
                }
                if (!keepFirmware(firmware, release.file.fileName(), &kept, &problem)
                    || !chooseStockFirmware(kept, &problem)) {
                    emit stockDownloadFailed(problem);
                    return;
                }
                emit stockDownloaded(kept);
            });
    connect(m_lynDownloader, &Ctr2MaxStockDownloader::failed, this, [this](const QString& message) {
        // A missing or unsigned list is not news while nothing was asked for.
        if (m_stockDownloadAsked)
            emit stockDownloadFailed(message);
        m_stockDownloadAsked = false;
    });
    connect(m_lynDownloader, &Ctr2MaxStockDownloader::releaseReady, this,
            [this] { m_stockDownloadAsked = false; });

    m_loaders = [] {
        QList<Loader> out;
#ifdef HAVE_SERIALPORT
        for (const EspFlasher::Device& d : EspFlasher::listDevices())
            out.append({d.portName, d.serialNumber.toUpper()});   // as HID's: upper case
#endif
        return out;
    };
    m_hid = [this] {
        if (m_relay)
            return m_relay->availableUsbDevices();
#ifdef HAVE_HIDAPI
        return Ctr2HidapiPort::enumerate();
#else
        return QList<Ctr2HidPort::DeviceInfo>();
#endif
    };
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &FirmwareInstallModel::poll);

    connect(m_downloader, &EspFirmwareDownloader::catalogReady, this,
            [this](const EspFirmwareCatalog& catalog) {
        m_catalog = catalog;
        m_releases.clear();
        for (const EspFirmwareCatalog::Product& p : catalog.products) {
            // The hardware it is for, else the chip.
            const QString target = p.device.isEmpty() ? p.chip : p.device;
            m_releases.append(target.isEmpty() ? tr("%1 %2").arg(p.name, p.version)
                                               : tr("%1 %2 for %3").arg(p.name, p.version, target));
        }
        m_catalogProblem = catalog.products.isEmpty() ? tr("No firmware available") : QString();
        emit releasesChanged();
    });
    connect(m_downloader, &EspFirmwareDownloader::releaseReady, this,
            [this](const EspFirmwareManifest& manifest, const QHash<QString, QByteArray>& files) {
        setReleaseForTesting(manifest, files);
    });
    connect(m_downloader, &EspFirmwareDownloader::progress, this,
            [this](const QString& step, qint64, qint64) {
        m_releaseText = step;
        emit releaseTextChanged();
    });
    connect(m_downloader, &EspFirmwareDownloader::failed, this, [this](const QString& message) {
        if (m_catalog.products.isEmpty() && !m_haveRelease) {
            m_catalogProblem = tr("No firmware list");
            emit releasesChanged();
        }
        m_releaseText = message;
        emit releaseTextChanged();
    });

    connect(m_job, &EspFirmwareJob::connected, this, &FirmwareInstallModel::connected);
    connect(m_job, &EspFirmwareJob::progress, this, &FirmwareInstallModel::progress);
    connect(m_job, &EspFirmwareJob::finished, this, &FirmwareInstallModel::onJobFinished);
    if (m_relay) {
        connect(m_relay, &Ctr2ProxyModel::firmwareUpdateStep, this,
                &FirmwareInstallModel::stepChanged);
        connect(m_relay, &Ctr2ProxyModel::firmwareUpdateProgress, this,
                [this](qint64 stored, qint64 total) {
                    emit progress(QString(), stored, total);
                });
        connect(m_relay, &Ctr2ProxyModel::firmwareUpdateFinished, this,
                [this](bool ok, const QString& message) {
                    if (!m_linkRunning)
                        return;   // not started here
                    m_linkRunning = false;
                    if (m_waiting && ok) {
                        // Restarted into download mode: the install starts
                        // once it is found there.
                        m_poll->start();
                        poll();
                        emit busyChanged();
                        return;
                    }
                    m_waiting = false;
                    m_waitStart = {};
                    onJobFinished(ok, ok ? QString() : message);
                });
    }
}

FirmwareInstallModel::~FirmwareInstallModel()
{
    m_job->cancel();
}

void FirmwareInstallModel::begin()
{
    loadStoredFirmware();
    emit releasesChanged();
    // Only a list signed with a trusted Lynovation key is read, so without
    // one there is nothing to fetch.
    if (!Ctr2MaxStockCatalog::trustedKeys().isEmpty())
        m_lynDownloader->fetchCatalog();
    m_downloader->fetchCatalog();
    m_poll->start();
    poll();
}

void FirmwareInstallModel::setDeviceSources(LoaderSource loaders, HidSource hid)
{
    m_loaders = std::move(loaders);
    m_hid = std::move(hid);
    poll();
}

void FirmwareInstallModel::setJobTargetOpener(EspFirmwareJob::TargetOpener opener)
{
    m_job->setTargetOpener(std::move(opener));
}

void FirmwareInstallModel::setReleaseForTesting(const EspFirmwareManifest& manifest,
                                                const QHash<QString, QByteArray>& files)
{
    m_manifest = manifest;
    m_files = files;
    m_haveRelease = true;
    m_releaseText = tr("Ready: %1 %2 for %3.")
                        .arg(manifest.product, manifest.version,
                             manifest.device.isEmpty() ? manifest.chip : manifest.device);
    emit releaseTextChanged();
}

void FirmwareInstallModel::chooseRelease(int index)
{
    if (index < 0 || index >= m_catalog.products.size())
        return;
    m_haveRelease = false;
    m_releaseText = m_catalog.products[index].description;
    emit releaseTextChanged();
    m_downloader->fetchRelease(m_catalog.products[index].package,
                               m_catalog.products[index].version);
}

bool FirmwareInstallModel::working() const
{
    return m_job->isRunning() || (m_relay && m_relay->firmwareUpdateRunning());
}

bool FirmwareInstallModel::busy() const
{
    return working() || m_waiting;
}

QString FirmwareInstallModel::linkVersion() const
{
    return m_relay ? m_relay->usbFirmwareVersion() : QString();
}

void FirmwareInstallModel::poll()
{
    if (working())
        return;
    Device device = Device::None;
    QString name, port, mac, hidPath;
    int loaders = 0;
    const QList<Loader> found = m_loaders ? m_loaders() : QList<Loader>();
    if (!found.isEmpty()) {
        device = Device::Bootloader;
        port = found.first().port;
        mac = found.first().mac.toUpper();   // HID serials are upper case
        loaders = found.size();
    } else if (m_hid) {
        // A CTR2-Max running firmware: AetherCTR2, installed beside the
        // stock firmware, names itself and relays over USB; the stock
        // firmware reports ESP32S3_DEV. Other controllers (AetherKnob, M5Dial,
        // CTR2-MIDI) are not CTR2-Max units and are passed over. The relay's
        // selected device first: an update over the link goes to that one.
        QList<Ctr2HidPort::DeviceInfo> devices = m_hid();
        const QString selected = m_relay ? m_relay->usbDevicePath() : QString();
        std::stable_partition(devices.begin(), devices.end(),
                              [&selected](const Ctr2HidPort::DeviceInfo& d) {
                                  return !selected.isEmpty() && d.path == selected;
                              });
        for (const Ctr2HidPort::DeviceInfo& d : std::as_const(devices)) {
            const QString model = d.ctr2Model();
            const bool aetherCtr2 = d.usbLink() && !model.isEmpty() && model == d.product;
            const bool stock = d.product == QLatin1String(kStockProduct) && !model.isEmpty();
            if (!aetherCtr2 && !stock)
                continue;
            device = aetherCtr2 ? Device::LinkFirmware : Device::Stock;
            name = aetherCtr2 ? model : QString();
            hidPath = d.path;
            if (d.serial.size() == 12)
                mac = QString::fromLatin1(QByteArray::fromHex(d.serial.toLatin1()).toHex(':')).toUpper();
            break;
        }
    }
    if (device != m_device || name != m_deviceName || port != m_port || mac != m_mac
        || hidPath != m_hidPath || loaders != m_loaderCount) {
        m_device = device;
        m_deviceName = name;
        m_port = port;
        m_mac = mac;
        m_hidPath = hidPath;
        m_loaderCount = loaders;
        emit deviceChanged();
    }
    startWhenInDownloadMode();
}

bool FirmwareInstallModel::stillTarget(const Target& target, QString* problem)
{
    // Look again now: a prompt or file picker may have been open for a while.
    poll();
    if (target.device == Device::None || target.device != m_device || target.port != m_port
        || target.mac != m_mac || target.hidPath != m_hidPath) {
        *problem = tr("The connected device changed while you were choosing. Nothing was "
                      "written; check it and try again.");
        return false;
    }
    return true;
}

bool FirmwareInstallModel::runJob(const Target& target, EspFirmwareJob::Action action,
                                  const QString& backupPath, const QString& restoreFrom,
                                  QString* problem, const EspFirmwareJob::Request::Plan& plan)
{
    if (busy()) {
        *problem = tr("Wait for the current step to finish.");
        return false;
    }
    if (target.device != Device::Bootloader) {
        *problem = tr("Put the CTR2-Max in download mode first.");
        return false;
    }
    if (!stillTarget(target, problem))
        return false;
    const QByteArray mac =
        QByteArray::fromHex(QString(target.mac).remove(QLatin1Char(':')).toLatin1());
    if (mac.size() != 6) {
        // Without it the job could not tell this device from another on
        // the same port.
        *problem = tr("The device's MAC address is not known. Nothing was written.");
        return false;
    }
    EspFirmwareJob::Request r;
    r.portName = target.port;
    r.expectedMac = mac;
    r.action = action;
    r.manifest = m_manifest;
    r.files = m_files;
    r.backupPath = backupPath;
    r.restoreFrom = restoreFrom;
    if (plan) {
        // The CTR2-Max's own firmware, laid out by the unit's table.
        r.manifest = {};
        r.files = {};
        r.plan = plan;
    } else {
        r.requireCtr2MaxRegistration = true;   // AetherCTR2, on registered CTR2-Max units only
    }
    if (!m_job->start(r)) {
        *problem = tr("Wait for the current step to finish.");
        return false;
    }
    m_poll->stop();
    emit busyChanged();
    return true;
}

bool FirmwareInstallModel::install(const Target& target, const QString& backupPath, QString* problem)
{
    if (!m_haveRelease) {
        *problem = tr("Choose a release first.");
        return false;
    }
    return runJob(target, EspFirmwareJob::Action::Install, backupPath, {}, problem);
}

bool FirmwareInstallModel::startFirmware(const Target& target, QString* problem)
{
    if (!m_haveRelease) {
        *problem = tr("Choose a release first.");
        return false;
    }
    return runJob(target, EspFirmwareJob::Action::Start, {}, {}, problem);
}

bool FirmwareInstallModel::backup(const Target& target, const QString& path, QString* problem)
{
    return runJob(target, EspFirmwareJob::Action::Backup, path, {}, problem);
}

bool FirmwareInstallModel::restore(const Target& target, const QString& backupPath, QString* problem)
{
    return runJob(target, EspFirmwareJob::Action::Restore, {}, backupPath, problem);
}

QByteArray FirmwareInstallModel::appImage() const
{
    // The application image is the one the manifest puts in one of the
    // firmware's own slots; the device picks the slot itself. A release with
    // more than one is not one the link can install.
    QByteArray app;
    int apps = 0;
    for (const EspFirmwareManifest::Image& image : m_manifest.images) {
        for (int slot : m_manifest.ownSlots) {
            if (image.offset == m_manifest.appSlots.value(slot)) {
                app = m_files.value(image.file);
                ++apps;
            }
        }
    }
    return apps == 1 ? app : QByteArray();
}

bool FirmwareInstallModel::linkImage(Ctr2FirmwareUpdater::Image* image, QString* problem) const
{
    const QByteArray app = appImage();
    if (!m_haveRelease || app.isEmpty()) {
        *problem = tr("This release cannot be installed over the USB cable. Put the device in "
                      "download mode to install it.");
        return false;
    }
    return Ctr2FirmwareUpdater::readImage(app, image, problem);
}

bool FirmwareInstallModel::canInstallOverLink(QString* problem) const
{
    Ctr2FirmwareUpdater::Image image;
    return linkImage(&image, problem);
}

bool FirmwareInstallModel::takesCommands(const Target& target) const
{
    return (target.device == Device::LinkFirmware || target.device == Device::Stock) && m_relay
           && !target.hidPath.isEmpty()
           && m_relay->usbDevicePath() == target.hidPath && m_relay->usbFirmwareCommands();
}

bool FirmwareInstallModel::relayHolds(const Target& target, QString* problem) const
{
    if (m_relay) {
        const QString path = m_relay->usbDevicePath();
        // Stock firmware reports no MAC: the relay's device is the one found.
        if (target.mac.isEmpty() && !target.hidPath.isEmpty() && path == target.hidPath)
            return true;
        for (const Ctr2HidPort::DeviceInfo& d : m_relay->availableUsbDevices()) {
            if (d.path != path || d.serial.size() != 12)
                continue;
            const QString mac =
                QString::fromLatin1(QByteArray::fromHex(d.serial.toLatin1()).toHex(':')).toUpper();
            if (mac == target.mac)
                return true;
        }
    }
    *problem = tr("The CTR2 relay has another device selected. Select %1 in the relay's USB list, "
                  "then try again.")
                   .arg(target.mac.isEmpty() ? tr("the CTR2-Max") : target.mac);
    return false;
}

bool FirmwareInstallModel::installOverLink(const Target& target, QString* problem)
{
    if (busy()) {
        *problem = tr("Wait for the current step to finish.");
        return false;
    }
    if (!m_relay || target.device != Device::LinkFirmware || !stillTarget(target, problem)) {
        if (problem->isEmpty())
            *problem = tr("The CTR2-Max is not running AetherCTR2.");
        return false;
    }
    Ctr2FirmwareUpdater::Image image;
    if (!relayHolds(target, problem) || !linkImage(&image, problem))
        return false;
    m_linkRunning = true;
    if (!m_relay->startFirmwareUpdate(image, problem)) {
        m_linkRunning = false;
        return false;
    }
    // An update that could not even open the device has already finished.
    if (m_relay->firmwareUpdateRunning()) {
        m_poll->stop();
        emit busyChanged();
    }
    return true;
}

bool FirmwareInstallModel::restart(const Target& target, bool bootloader, QString* problem)
{
    if (busy()) {
        *problem = tr("Wait for the current step to finish.");
        return false;
    }
    if (!m_relay || target.device == Device::None || target.device == Device::Bootloader
        || !stillTarget(target, problem)) {
        if (problem->isEmpty())
            *problem = tr("The CTR2-Max's firmware does not restart on request.");
        return false;
    }
    if (!relayHolds(target, problem))
        return false;
    // Firmware that takes the link's commands: AetherCTR2, or stock
    // firmware that offers them.
    if (!takesCommands(target)) {
        *problem = tr("The CTR2-Max's firmware does not restart on request.");
        return false;
    }
    m_linkRunning = true;
    if (!m_relay->restartUsbDevice(bootloader ? Ctr2FirmwareUpdater::Restart::Bootloader
                                              : Ctr2FirmwareUpdater::Restart::OtherFirmware,
                                   problem)) {
        m_linkRunning = false;
        return false;
    }
    if (m_relay->firmwareUpdateRunning()) {
        m_poll->stop();
        emit busyChanged();
    }
    return true;
}

namespace {

QString& firmwareFolderOverride()
{
    static QString folder;
    return folder;
}

QString nameOf(const Ctr2MaxStockFirmware& firmware, const QString& fileName)
{
    return firmware.version().isEmpty()
               ? QFileInfo(fileName).completeBaseName()
               : QCoreApplication::translate("AetherSDR::FirmwareInstallModel",
                                             "CTR2-Max firmware %1")
                     .arg(firmware.version());
}

bool readFirmware(const QString& path, Ctr2MaxStockFirmware* out, QString* problem)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *problem = QCoreApplication::translate("AetherSDR::FirmwareInstallModel",
                                               "Could not open %1.")
                       .arg(QFileInfo(path).fileName());
        return false;
    }
    if (f.size() > kMaxFirmwareFile) {
        *problem = QCoreApplication::translate("AetherSDR::FirmwareInstallModel",
                                               "%1 is too large to be CTR2-Max firmware.")
                       .arg(QFileInfo(path).fileName());
        return false;
    }
    return Ctr2MaxStockFirmware::read(f.readAll(), out, problem);
}

} // namespace

void FirmwareInstallModel::setFirmwareFolderForTesting(const QString& folder)
{
    firmwareFolderOverride() = folder;
}

QString FirmwareInstallModel::firmwareFolder()
{
    if (!firmwareFolderOverride().isEmpty())
        return firmwareFolderOverride();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("CTR2-Max Firmware"));
}

void FirmwareInstallModel::loadStoredFirmware()
{
    // Each kept .bin is read once, then known by its size and time: opening
    // the window or another file does not read them all again.
    m_stored.clear();
    const QDir dir(firmwareFolder());
    QHash<QString, KeptFile> seen;
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.bin")}, QDir::Files)) {
        KeptFile kept = m_keptFiles.value(info.filePath());
        if (kept.size != info.size() || kept.modified != info.lastModified()) {
            Ctr2MaxStockFirmware firmware;
            QString ignored;
            kept = {info.size(), info.lastModified(), false, {}};
            if (readFirmware(info.filePath(), &firmware, &ignored)) {
                kept.valid = true;
                kept.firmware = {info.filePath(), nameOf(firmware, info.fileName()),
                                 firmware.version()};
            }
        }
        seen.insert(info.filePath(), kept);
        if (kept.valid)
            m_stored.append(kept.firmware);
    }
    m_keptFiles = seen;
    std::stable_sort(m_stored.begin(), m_stored.end(),
                     [](const StoredFirmware& a, const StoredFirmware& b) {
                         return Ctr2MaxStockFirmware::newer(a.version, b.version);
                     });
}

bool FirmwareInstallModel::openStockFirmware(const QString& path, QString* problem)
{
    Ctr2MaxStockFirmware firmware;
    if (!readFirmware(path, &firmware, problem))
        return false;
    QString kept;
    return keepFirmware(firmware, QFileInfo(path).completeBaseName(), &kept, problem)
           && chooseStockFirmware(kept, problem);
}

bool FirmwareInstallModel::keepFirmware(const Ctr2MaxStockFirmware& firmware,
                                        const QString& fallbackName, QString* kept,
                                        QString* problem)
{
    // Kept as its version's .bin (the image itself, without the manuals the
    // download carries), replacing an earlier copy of the same version.
    const QString folder = firmwareFolder();
    const QString keptName =
        (firmware.version().isEmpty() ? QFileInfo(fallbackName).completeBaseName()
                                      : QStringLiteral("CTR2-Max_%1").arg(firmware.version()))
        + QStringLiteral(".bin");
    *kept = QDir(folder).filePath(keptName);
    QSaveFile out(*kept);
    if (!QDir().mkpath(folder) || !out.open(QIODevice::WriteOnly)
        || out.write(firmware.image()) != firmware.image().size() || !out.commit()) {
        *problem = tr("Could not keep a copy of it in %1.").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    loadStoredFirmware();
    return true;
}

void FirmwareInstallModel::downloadStockRelease(int index)
{
    if (index < 0 || index >= m_stockReleases.releases.size())
        return;
    m_stockDownloadAsked = true;
    m_lynDownloader->fetchRelease(m_stockReleases.releases[index]);
}

bool FirmwareInstallModel::chooseStockFirmware(const QString& path, QString* problem)
{
    Ctr2MaxStockFirmware firmware;
    if (!readFirmware(path, &firmware, problem))
        return false;
    m_stock = firmware;
    m_stockPath = path;
    m_haveStock = true;
    emit releasesChanged();
    return true;
}

QString FirmwareInstallModel::stockFirmwareName() const
{
    return nameOf(m_stock, m_stockPath);
}

namespace {
QString& backupFolderOverride()
{
    static QString folder;
    return folder;
}
} // namespace

void FirmwareInstallModel::setBackupFolderForTesting(const QString& folder)
{
    backupFolderOverride() = folder;
}

QString FirmwareInstallModel::backupFolder()
{
    if (!backupFolderOverride().isEmpty())
        return backupFolderOverride();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("CTR2-Max Backups"));
}

bool FirmwareInstallModel::whenInDownloadMode(const Target& target, Start start,
                                              QString* problem)
{
    if (busy()) {
        *problem = tr("Wait for the current step to finish.");
        return false;
    }
    if (target.device == Device::None) {
        *problem = tr("Connect the CTR2-Max with a USB cable.");
        return false;
    }
    // Firmware that takes the link's commands is asked to restart into
    // download mode; other firmware needs the unit's buttons, which the
    // window asks for. Waiting for those, the device found may already be
    // passing through a restart: the action goes to the one device found in
    // download mode, and the job names it before writing.
    const bool buttons = target.device != Device::Bootloader && !takesCommands(target);
    if (!buttons && !stillTarget(target, problem))
        return false;
    if (target.device == Device::Bootloader)
        return start(target, problem);
    if (!buttons) {
        if (!restart(target, true, problem))
            return false;
        if (!m_linkRunning)
            return true;   // the request failed at once, and said so
        emit stepChanged(tr("Restarting the CTR2-Max into download mode…"));
    } else {
        emit stepChanged(tr("Waiting for the CTR2-Max in download mode…"));
    }
    m_waiting = true;
    m_waitStart = std::move(start);
    m_waitSeen = 0;
    m_waitMac = target.mac;
    emit busyChanged();
    return true;
}

bool FirmwareInstallModel::installRelease(const Target& target, QString* problem)
{
    if (!m_haveRelease) {
        *problem = tr("Choose a release first.");
        return false;
    }
    return whenInDownloadMode(
        target, [this](const Target& t, QString* why) { return startInstall(t, {}, why); },
        problem);
}

bool FirmwareInstallModel::installStockFirmware(const Target& target, QString* problem)
{
    if (!m_haveStock) {
        *problem = tr("Open the CTR2-Max firmware file first.");
        return false;
    }
    const Ctr2MaxStockFirmware firmware = m_stock;
    const EspFirmwareJob::Request::Plan plan =
        [firmware](const QByteArray& table, quint32 flashSize, EspFirmwareManifest* manifest,
                   QHash<QString, QByteArray>* files, QString* why) {
            return firmware.plan(table, flashSize, manifest, files, why);
        };
    return whenInDownloadMode(
        target, [this, plan](const Target& t, QString* why) { return startInstall(t, plan, why); },
        problem);
}

bool FirmwareInstallModel::restartInto(const Target& target, Firmware firmware, QString* problem)
{
    const bool running = (firmware == Firmware::AetherCtr2 && target.device == Device::LinkFirmware)
                         || (firmware == Firmware::Stock && target.device == Device::Stock)
                         || (firmware == Firmware::Bootloader
                             && target.device == Device::Bootloader);
    if (running) {
        *problem = tr("The CTR2-Max already runs it.");
        return false;
    }
    // Firmware that takes the link's commands (AetherCTR2, and stock
    // firmware offering them) restarts on request: into the other firmware
    // (SWITCH) or download mode.
    if (takesCommands(target))
        return restart(target, firmware == Firmware::Bootloader, problem);
    if (firmware == Firmware::Bootloader) {
        *problem = tr("Put the CTR2-Max in download mode with its BOOT and RESET buttons.");
        return false;
    }
    const bool aetherCtr2 = firmware == Firmware::AetherCtr2;
    return whenInDownloadMode(
        target,
        [this, aetherCtr2](const Target& t, QString* why) {
            const EspFirmwareJob::Request::Plan plan =
                [aetherCtr2](const QByteArray& table, quint32 flashSize,
                             EspFirmwareManifest* manifest, QHash<QString, QByteArray>* files,
                             QString* problem) {
                    files->clear();
                    return Ctr2MaxStockFirmware::planStart(table, flashSize, aetherCtr2, manifest,
                                                           problem);
                };
            return runJob(t, EspFirmwareJob::Action::Start, {}, {}, why, plan);
        },
        problem);
}

bool FirmwareInstallModel::restoreBackup(const Target& target, const QString& path,
                                         QString* problem)
{
    return whenInDownloadMode(
        target,
        [this, path](const Target& t, QString* why) {
            return runJob(t, EspFirmwareJob::Action::Restore, {}, path, why);
        },
        problem);
}

void FirmwareInstallModel::startWhenInDownloadMode()
{
    if (!m_waiting || working() || m_device != Device::Bootloader || m_loaderCount != 1
        || (!m_waitMac.isEmpty() && m_mac.compare(m_waitMac, Qt::CaseInsensitive) != 0)) {
        m_waitSeen = 0;
        return;
    }
    // Seen on two polls in a row: a port that has just appeared may not have
    // its permissions yet (udev sets them a moment after it appears).
    if (++m_waitSeen < 2)
        return;
    m_waitSeen = 0;
    m_waiting = false;
    const Start start = std::exchange(m_waitStart, {});
    QString problem;
    if (!start(target(), &problem)) {
        emit busyChanged();
        emit finished(false, problem);
    }
}

bool FirmwareInstallModel::startInstall(const Target& target,
                                        const EspFirmwareJob::Request::Plan& plan,
                                        QString* problem)
{
    const QString folder = backupFolder();
    if (!QDir().mkpath(folder)) {
        *problem = tr("Could not make the backup folder %1. Nothing was written.").arg(folder);
        return false;
    }
    const QString backup = QDir(folder).filePath(
        QStringLiteral("flash-backup-%1-%2.bin")
            .arg(QString(target.mac).remove(QLatin1Char(':')),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (!runJob(target, EspFirmwareJob::Action::Install, backup, {}, problem, plan))
        return false;
    m_lastBackup = backup;
    m_installRunning = true;
    m_waitMac = target.mac;
    return true;
}

QList<FirmwareInstallModel::Backup> FirmwareInstallModel::backups() const
{
    // Every backup with its description beside it (<backup>.json, which
    // names the device): those made here, and those saved by hand with Back
    // Up in earlier versions, which suggested the Documents folder itself.
    // A restore checks the file against its description, and the device.
    const QString want = m_mac.toUpper();
    QList<Backup> out;
    QStringList seen;
    const QDir folder(backupFolder());
    for (const QDir& dir : {folder, QDir(QFileInfo(folder.path()).path())}) {
        for (const QFileInfo& file : dir.entryInfoList({QStringLiteral("*.bin")}, QDir::Files)) {
            const QString path = file.absoluteFilePath();
            if (seen.contains(path) || !QFile::exists(path + QStringLiteral(".json")))
                continue;
            seen.append(path);
            EspFirmwareJob::BackupInfo info;
            QString ignored;
            if (!EspFirmwareJob::readBackupInfo(path, &info, &ignored))
                continue;
            const QString mac = QString::fromLatin1(info.mac.toHex(':')).toUpper();
            if (!want.isEmpty() && mac != want)
                continue;
            out.append({path, mac, file.lastModified()});
        }
    }
    // Newest first, across devices too.
    std::stable_sort(out.begin(), out.end(),
                     [](const Backup& a, const Backup& b) { return a.made > b.made; });
    return out;
}

void FirmwareInstallModel::pruneBackups(const QString& mac)
{
    // Only the backups made here, by name: flash-backup-<MAC>-<date>-<time>.bin.
    const QString id = QString(mac).remove(QLatin1Char(':'));
    const QRegularExpression made(QStringLiteral("^flash-backup-%1-\\d{8}-\\d{6}\\.bin$")
                                      .arg(QRegularExpression::escape(id)));
    QDir dir(backupFolder());
    QStringList backups;
    for (const QString& name : dir.entryList({QStringLiteral("flash-backup-*.bin")}, QDir::Files)) {
        if (made.match(name).hasMatch())
            backups.append(name);
    }
    std::sort(backups.begin(), backups.end(), std::greater<>());   // newest first
    for (qsizetype i = kKeptBackups; i < backups.size(); ++i) {
        dir.remove(backups[i]);
        dir.remove(backups[i] + QStringLiteral(".json"));
    }
}

void FirmwareInstallModel::cancel()
{
    if (m_waiting && !working()) {
        m_waiting = false;
        m_waitStart = {};
        emit busyChanged();
        emit finished(false, tr("Cancelled. Nothing was written."));
        return;
    }
    m_waiting = false;
    m_waitStart = {};
    if (m_relay && m_relay->firmwareUpdateRunning())
        m_relay->cancelFirmwareUpdate();
    m_job->cancel();
}

void FirmwareInstallModel::onJobFinished(bool ok, const QString& message)
{
    if (m_installRunning) {
        m_installRunning = false;
        if (ok)
            pruneBackups(m_waitMac);
    }
    m_poll->start();
    poll();
    emit busyChanged();
    emit finished(ok, message);
}

} // namespace AetherSDR
