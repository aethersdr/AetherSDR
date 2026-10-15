#include "models/FirmwareInstallModel.h"

#include "core/Ctr2FirmwareUpdater.h"
#include "models/Ctr2ProxyModel.h"
#ifdef HAVE_SERIALPORT
#include "core/EspFlasher.h"
#endif
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QTimer>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kPollMs = 1000;
// The USB product string of the CTR2-Max's stock firmware.
constexpr char kStockProduct[] = "ESP32S3_DEV";

} // namespace

FirmwareInstallModel::FirmwareInstallModel(Ctr2ProxyModel* relay, QObject* parent)
    : QObject(parent)
    , m_relay(relay)
    , m_downloader(new EspFirmwareDownloader(this))
    , m_job(new EspFirmwareJob(this))
    , m_poll(new QTimer(this))
{
    m_loaders = [] {
        QList<Loader> out;
#ifdef HAVE_SERIALPORT
        for (const EspFlasher::Device& d : EspFlasher::listDevices())
            out.append({d.portName, d.serialNumber});
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

bool FirmwareInstallModel::busy() const
{
    return m_job->isRunning() || (m_relay && m_relay->firmwareUpdateRunning());
}

QString FirmwareInstallModel::linkVersion() const
{
    return m_relay ? m_relay->usbFirmwareVersion() : QString();
}

void FirmwareInstallModel::poll()
{
    if (busy())
        return;
    Device device = Device::None;
    QString name, port, mac;
    int loaders = 0;
    const QList<Loader> found = m_loaders ? m_loaders() : QList<Loader>();
    if (!found.isEmpty()) {
        device = Device::Bootloader;
        port = found.first().port;
        mac = found.first().mac;
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
            if (d.serial.size() == 12)
                mac = QString::fromLatin1(QByteArray::fromHex(d.serial.toLatin1()).toHex(':')).toUpper();
            break;
        }
    }
    if (device == m_device && name == m_deviceName && port == m_port && mac == m_mac
        && loaders == m_loaderCount)
        return;
    m_device = device;
    m_deviceName = name;
    m_port = port;
    m_mac = mac;
    m_loaderCount = loaders;
    emit deviceChanged();
}

bool FirmwareInstallModel::stillTarget(const Target& target, QString* problem)
{
    // Look again now: a prompt or file picker may have been open for a while.
    poll();
    if (target.device == Device::None || target.device != m_device || target.port != m_port
        || target.mac != m_mac) {
        *problem = tr("The connected device changed while you were choosing. Nothing was "
                      "written; check it and try again.");
        return false;
    }
    return true;
}

bool FirmwareInstallModel::runJob(const Target& target, EspFirmwareJob::Action action,
                                  const QString& backupPath, const QString& restoreFrom,
                                  QString* problem)
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
    r.requireCtr2MaxRegistration = true;   // this window installs AetherCTR2 on CTR2-Max units
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

bool FirmwareInstallModel::relayHolds(const Target& target, QString* problem) const
{
    if (m_relay) {
        const QString path = m_relay->usbDevicePath();
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
                   .arg(target.mac);
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
    if (!m_relay || target.device != Device::LinkFirmware || !stillTarget(target, problem)) {
        if (problem->isEmpty())
            *problem = tr("The CTR2-Max is not running AetherCTR2.");
        return false;
    }
    if (!relayHolds(target, problem))
        return false;
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

void FirmwareInstallModel::cancel()
{
    if (m_relay && m_relay->firmwareUpdateRunning())
        m_relay->cancelFirmwareUpdate();
    m_job->cancel();
}

void FirmwareInstallModel::onJobFinished(bool ok, const QString& message)
{
    m_poll->start();
    poll();
    emit busyChanged();
    emit finished(ok, message);
}

} // namespace AetherSDR
