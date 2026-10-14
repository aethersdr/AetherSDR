#include "FirmwareInstallDialog.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"
#include "ScopedChildWidget.h"
#include "core/ThemeManager.h"
#include "core/Ctr2FirmwareUpdater.h"
#include "core/EspFirmwareJob.h"
#include "models/Ctr2ProxyModel.h"
#ifdef HAVE_SERIALPORT
#include "core/EspFlasher.h"
#endif
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QCloseEvent>
#include <QListWidget>
#include <QSignalBlocker>
#include <QFileInfo>
#include <QDate>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kPollMs = 1000;

QLabel* section(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("SectionLabel"));
    return l;
}

QLabel* wrapped(const QString& text = {})
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    return l;
}

} // namespace

FirmwareInstallDialog::FirmwareInstallDialog(Ctr2ProxyModel* model, QWidget* parent)
    : CanonWindow(tr("CTR2-Max Firmware"), parent)
    , m_model(model)
    , m_downloader(new EspFirmwareDownloader(this))
    , m_job(new EspFirmwareJob(this))
    , m_poll(new QTimer(this))
{
    setObjectName(QStringLiteral("firmwareInstallDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(520, 560);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
               + QStringLiteral(
                   "QListWidget#firmwareInstallProduct { background: {{color.canon.nested}};"
                   " color: {{color.canon.ink}}; border: 1px solid {{color.canon.line}};"
                   " border-radius: 6px; padding: 4px; outline: 0; }"
                   "QListWidget#firmwareInstallProduct::item { padding: 5px 8px; border-radius: 4px; }"
                   "QListWidget#firmwareInstallProduct::item:hover { background: {{color.canon.raised}}; }"
                   "QListWidget#firmwareInstallProduct::item:selected {"
                   " background: {{color.canon.control}}; color: {{color.canon.cyan}}; }");
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    root->addWidget(makeCanonHeader(tr("CTR2-Max Firmware")));

    root->addWidget(section(tr("1. Firmware")));
    // An inline list, not a drop-down: it stays inside the dialog's sheet.
    m_product = new QListWidget;
    m_product->setObjectName(QStringLiteral("firmwareInstallProduct"));
    m_product->setAccessibleName(tr("Firmware to install"));
    m_product->setSelectionMode(QAbstractItemView::SingleSelection);
    m_product->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_product->setFixedHeight(m_product->fontMetrics().height() * 3 + 34);
    root->addWidget(m_product);
    showPlaceholder(tr("Looking for firmware…"));
    m_release = wrapped();
    m_release->setObjectName(QStringLiteral("firmwareInstallRelease"));
    root->addWidget(m_release);

    root->addWidget(section(tr("2. Device")));
    root->addWidget(wrapped(tr(
        "Connect the CTR2-Max with a USB cable. To install from its stock firmware, put it in "
        "download mode: hold BOOT (through the hole in the back), press and release RESET, then "
        "release BOOT. Its screen goes dark until the install restarts it.")));
    // The device's mode as a badge of its own; details in its tooltip.
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    auto* statusLabel = new QLabel(tr("Status"));
    statusLabel->setObjectName(QStringLiteral("SectionLabel"));
    statusRow->addWidget(statusLabel);
    m_badge = new QLabel;
    m_badge->setObjectName(QStringLiteral("firmwareInstallStatus"));
    m_badge->setAccessibleName(tr("Device status"));
    m_badge->setAlignment(Qt::AlignCenter);
    m_badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusRow->addWidget(m_badge);
    statusRow->addStretch(1);
    root->addSpacing(4);
    root->addLayout(statusRow);
    root->addSpacing(4);

    root->addWidget(section(tr("3. Action")));
    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    const auto button = [&](const QString& text, const QString& name, const QString& tip) {
        auto* b = new QPushButton(text);
        b->setObjectName(name);
        b->setToolTip(tip);
        actions->addWidget(b);
        return b;
    };
    m_install = button(tr("Install…"), QStringLiteral("firmwareInstallInstall"),
                       tr("Back up the device's whole flash to a file, then install the firmware "
                          "beside what the device already has."));
    m_start = button(tr("Start"), QStringLiteral("firmwareInstallStart"),
                     tr("Restart the device into this firmware, already installed on it. "
                        "Nothing else is written."));
    m_backup = button(tr("Back Up…"), QStringLiteral("firmwareInstallBackup"),
                      tr("Save the device's whole flash to a file."));
    m_restore = button(tr("Restore…"), QStringLiteral("firmwareInstallRestore"),
                       tr("Write a backup made here back onto the same device."));
    actions->addStretch(1);
    root->addLayout(actions);

    m_step = wrapped();
    m_step->setObjectName(QStringLiteral("firmwareInstallStep"));
    m_step->setAccessibleName(tr("Install status"));
    root->addWidget(m_step);
    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("firmwareInstallProgress"));
    m_bar->setAccessibleName(tr("Install progress"));
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_bar->hide();
    root->addWidget(m_bar);
    root->addStretch(1);

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("firmwareInstallCancel"));
    m_cancel->hide();
    closeRow->addWidget(m_cancel);
    auto* close = new QPushButton(tr("Close"));
    close->setObjectName(QStringLiteral("firmwareInstallClose"));
    closeRow->addWidget(close);
    root->addLayout(closeRow);

    connect(m_product, &QListWidget::currentRowChanged, this, &FirmwareInstallDialog::onProductChosen);
    connect(m_install, &QPushButton::clicked, this, &FirmwareInstallDialog::onInstall);
    connect(m_start, &QPushButton::clicked, this, &FirmwareInstallDialog::onStart);
    connect(m_backup, &QPushButton::clicked, this, &FirmwareInstallDialog::onBackup);
    connect(m_restore, &QPushButton::clicked, this, &FirmwareInstallDialog::onRestore);
    connect(m_cancel, &QPushButton::clicked, this, &FirmwareInstallDialog::onCancel);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(m_downloader, &EspFirmwareDownloader::catalogReady, this,
            &FirmwareInstallDialog::onCatalogReady);
    connect(m_downloader, &EspFirmwareDownloader::releaseReady, this,
            &FirmwareInstallDialog::onReleaseReady);
    connect(m_downloader, &EspFirmwareDownloader::progress, this,
            [this](const QString& step, qint64, qint64) { m_release->setText(step); });
    connect(m_downloader, &EspFirmwareDownloader::failed, this, [this](const QString& message) {
        if (m_catalog.products.isEmpty() && !m_haveRelease)
            showPlaceholder(tr("No firmware list"));
        m_release->setText(message);
        sync();
    });
    connect(m_job, &EspFirmwareJob::connected, this,
            [this](const QString& chip, const QByteArray& mac, quint32 flashSize) {
                showStatus(tr("Bootloader"), "aqua",
                           tr("Connected: %1, %2 MB flash, %3")
                               .arg(chip)
                               .arg(flashSize >> 20)
                               .arg(QString::fromLatin1(mac.toHex(':')).toUpper()));
            });
    connect(m_job, &EspFirmwareJob::progress, this, &FirmwareInstallDialog::onProgress);
    connect(m_job, &EspFirmwareJob::finished, this, &FirmwareInstallDialog::onFinished);
    connect(m_poll, &QTimer::timeout, this, &FirmwareInstallDialog::pollDevices);
    if (m_model) {
        connect(m_model, &Ctr2ProxyModel::firmwareUpdateStep, this,
                [this](const QString& step) { m_step->setText(step); });
        connect(m_model, &Ctr2ProxyModel::firmwareUpdateProgress, this,
                [this](qint64 stored, qint64 total) {
                    m_bar->setValue(total > 0 ? int(stored * 100 / total) : 0);
                });
        connect(m_model, &Ctr2ProxyModel::firmwareUpdateFinished, this,
                [this](bool ok, const QString& message) {
                    if (!m_jobDone.isEmpty())   // started here
                        onFinished(ok, message);
                });
    }

    m_downloader->fetchCatalog();
    m_poll->start(kPollMs);
    pollDevices();
}

FirmwareInstallDialog::~FirmwareInstallDialog()
{
    m_job->cancel();
}

bool FirmwareInstallDialog::busy() const
{
    return m_job->isRunning() || (m_model && m_model->firmwareUpdateRunning());
}

bool FirmwareInstallDialog::linkUpdateAvailable() const
{
    // The relay's USB device reports a firmware version only when it takes
    // updates over the link.
    return m_model && m_model->transport() == Ctr2ProxyModel::Transport::Usb
           && m_model->firmwareUpdateAvailable() && !m_model->usbDevicePath().isEmpty()
           && !m_model->usbFirmwareVersion().isEmpty();
}

void FirmwareInstallDialog::closeEvent(QCloseEvent* event)
{
    if (busy()) {
        m_step->setText(tr("Wait for this step to finish, or press Cancel, before closing."));
        event->ignore();
        return;
    }
    CanonWindow::closeEvent(event);
}

void FirmwareInstallDialog::reject()
{
    if (busy()) {
        m_step->setText(tr("Wait for this step to finish, or press Cancel, before closing."));
        return;
    }
    CanonWindow::reject();
}

void FirmwareInstallDialog::showPlaceholder(const QString& text)
{
    const QSignalBlocker block(m_product);
    m_product->clear();
    auto* item = new QListWidgetItem(text, m_product);
    item->setFlags(Qt::NoItemFlags);   // not a choice
}

void FirmwareInstallDialog::onCatalogReady(const EspFirmwareCatalog& catalog)
{
    m_catalog = catalog;
    const QSignalBlocker block(m_product);
    m_product->clear();
    for (const EspFirmwareCatalog::Product& p : catalog.products) {
        // The hardware it is for, else the chip.
        const QString target = p.device.isEmpty() ? p.chip : p.device;
        m_product->addItem(target.isEmpty() ? tr("%1 %2").arg(p.name, p.version)
                                            : tr("%1 %2 for %3").arg(p.name, p.version, target));
    }
    if (catalog.products.isEmpty())
        showPlaceholder(tr("No firmware available"));
    sync();
}

void FirmwareInstallDialog::onProductChosen(int index)
{
    if (index < 0 || index >= m_catalog.products.size())
        return;
    m_haveRelease = false;
    m_release->setText(m_catalog.products[index].description);
    m_downloader->fetchRelease(m_catalog.products[index].manifest,
                               m_catalog.products[index].version);
    sync();
}

void FirmwareInstallDialog::onReleaseReady(const EspFirmwareManifest& manifest,
                                           const QHash<QString, QByteArray>& files)
{
    m_manifest = manifest;
    m_files = files;
    m_haveRelease = true;
    m_release->setText(tr("Ready: %1 %2 for %3.")
                           .arg(manifest.product, manifest.version,
                                manifest.device.isEmpty() ? manifest.chip : manifest.device));
    sync();
}

void FirmwareInstallDialog::showStatus(const QString& state, const char* colorToken,
                                       const QString& detail)
{
    const QString color = ThemeManager::instance()
                              .color(QStringLiteral("color.canon.") + QLatin1String(colorToken))
                              .name();
    const QString ground =
        ThemeManager::instance().color(QStringLiteral("color.canon.nested")).name();
    m_badge->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %1; "
                                          "border-radius: 11px; padding: 3px 12px; font-weight: 600; }")
                               .arg(color, ground));
    m_badge->setText(state);
    m_badge->setToolTip(detail);
    m_badge->setAccessibleDescription(detail);
}

void FirmwareInstallDialog::pollDevices()
{
    if (busy())
        return;
    m_port.clear();
    m_deviceMac.clear();
    m_overLink = false;
#ifdef HAVE_SERIALPORT
    const QList<EspFlasher::Device> loaders = EspFlasher::listDevices();
    if (!loaders.isEmpty()) {
        m_port = loaders.first().portName;
        m_deviceMac = loaders.first().serialNumber;
        QString detail = tr("%1. Its screen stays dark until it restarts; that is normal.")
                             .arg(m_deviceMac);
        if (loaders.size() > 1)
            detail += QLatin1Char(' ')
                      + tr("%1 devices are in download mode; connect only the one to install.")
                            .arg(loaders.size());
        showStatus(tr("Bootloader"), "aqua", detail);
        sync();
        return;
    }
#endif
#ifdef HAVE_HIDAPI
    // A controller running firmware. Firmware installed beside the device's
    // own names itself (a self-named CTR2 device: its product string is its
    // name); a device reporting one of the fixed CTR2 product strings runs
    // its maker's firmware.
    for (const Ctr2HidPort::DeviceInfo& d : Ctr2HidapiPort::enumerate()) {
        const QString model = d.ctr2Model();
        if (model.isEmpty())
            continue;
        const bool selfNamed = model == d.product;
        m_overLink = selfNamed && linkUpdateAvailable();
        QString detail;
        if (d.serial.size() == 12)
            detail = QString::fromLatin1(QByteArray::fromHex(d.serial.toLatin1()).toHex(':')).toUpper()
                     + QStringLiteral(". ");
        detail += m_overLink ? tr("Running %1; Install updates it over the USB cable.")
                                   .arg(m_model->usbFirmwareVersion())
                             : tr("Put it in download mode to install.");
        showStatus(selfNamed ? model : tr("Stock firmware"), selfNamed ? "cyan" : "sparkGold",
                   detail);
        sync();
        return;
    }
#endif
    showStatus(tr("No CTR2-Max"), "muted", tr("Connect the CTR2-Max with a USB cable."));
    sync();
}

void FirmwareInstallDialog::sync()
{
    const bool running = busy();
    const bool loader = !m_port.isEmpty();
    m_install->setEnabled(!running && (loader || m_overLink) && m_haveRelease);
    m_start->setEnabled(!running && loader && m_haveRelease);
    m_backup->setEnabled(!running && loader);
    m_restore->setEnabled(!running && loader);
    m_product->setEnabled(!running);
    m_cancel->setVisible(running);
    m_bar->setVisible(running);
}

QString FirmwareInstallDialog::suggestBackupPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString mac = QString(m_deviceMac).remove(QLatin1Char(':'));
    return QDir(dir).filePath(QStringLiteral("flash-backup-%1-%2.bin")
                                  .arg(mac, QDate::currentDate().toString(Qt::ISODate)));
}

void FirmwareInstallDialog::onInstall()
{
    if (m_port.isEmpty() && m_overLink) {
        installOverLink();
        return;
    }
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Question, tr("Install %1?").arg(m_manifest.product),
        tr("Install %1 %2 on the CTR2-Max %3?").arg(m_manifest.product, m_manifest.version, m_deviceMac),
        QMessageBox::Cancel, this);
    ask.get()->setInformativeText(
        tr("A backup saves the device's whole flash to a file first, so it can be put back exactly "
           "as it is now; it takes about a minute. Either way, the install writes only what the "
           "firmware's manifest lists, and never what it protects."));
    QPushButton* withBackup =
        ask.get()->addButton(tr("Back Up and Install…"), QMessageBox::AcceptRole);
    QPushButton* without = ask.get()->addButton(tr("Install Without Backup"), QMessageBox::AcceptRole);
    ask.get()->setDefaultButton(withBackup);
    ask.get()->exec();
    QString path;
    if (ask.get()->clickedButton() == withBackup) {
        path = QFileDialog::getSaveFileName(this, tr("Save the backup"), suggestBackupPath(),
                                            tr("Flash backup (*.bin)"));
        if (path.isEmpty())
            return;
        m_jobDone = tr("%1 %2 is installed and starting. The backup is in %3.")
                        .arg(m_manifest.product, m_manifest.version, path);
    } else if (ask.get()->clickedButton() == without) {
        m_jobDone = tr("%1 %2 is installed and starting.").arg(m_manifest.product, m_manifest.version);
    } else {
        return;
    }
    runJob(int(EspFirmwareJob::Action::Install), path, {});
}

void FirmwareInstallDialog::installOverLink()
{
    // The application image is the one the manifest puts in one of the
    // firmware's own slots; the device picks the slot itself.
    QByteArray app;
    for (const EspFirmwareManifest::Image& image : m_manifest.images) {
        for (int slot : m_manifest.ownSlots) {
            if (image.offset == m_manifest.appSlots.value(slot))
                app = m_files.value(image.file);
        }
    }
    Ctr2FirmwareUpdater::Image image;
    QString problem;
    if (app.isEmpty())
        problem = tr("This release cannot be installed over the USB cable. Put the device in "
                     "download mode to install it.");
    else
        Ctr2FirmwareUpdater::readImage(app, &image, &problem);
    if (!problem.isEmpty()) {
        m_step->setText(problem);
        return;
    }
    const QString current = m_model->usbFirmwareVersion();
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Question, tr("Install %1?").arg(m_manifest.product),
        tr("Install %1 %2 over the USB cable? The device runs %3 now.")
            .arg(m_manifest.product, m_manifest.version, current),
        QMessageBox::Cancel, this);
    ask.get()->setInformativeText(
        tr("It takes about three minutes. The device checks the firmware's signature, and keeps "
           "the firmware it has if anything goes wrong."));
    QPushButton* go = ask.get()->addButton(tr("Install"), QMessageBox::AcceptRole);
    ask.get()->setDefaultButton(go);
    ask.get()->exec();
    if (ask.get()->clickedButton() != go || !m_model)
        return;
    m_jobDone = tr("%1 %2 is installed and running.").arg(m_manifest.product, m_manifest.version);
    m_bar->setValue(0);
    m_step->setText(tr("Starting the update…"));
    if (!m_model->startFirmwareUpdate(image, &problem)) {
        m_jobDone.clear();
        m_step->setText(problem);
        return;
    }
    m_poll->stop();
    sync();
}

void FirmwareInstallDialog::onStart()
{
    m_jobDone = tr("The device is starting %1.").arg(m_manifest.product);
    runJob(int(EspFirmwareJob::Action::Start), {}, {});
}

void FirmwareInstallDialog::onBackup()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the backup"), suggestBackupPath(),
                                                      tr("Flash backup (*.bin)"));
    if (path.isEmpty())
        return;
    m_jobDone = tr("Backed up to %1. The device is restarting.").arg(path);
    runJob(int(EspFirmwareJob::Action::Backup), path, {});
}

void FirmwareInstallDialog::onRestore()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a backup"), suggestBackupPath(),
                                                      tr("Flash backup (*.bin)"));
    if (path.isEmpty())
        return;
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Warning, tr("Restore the backup?"),
        tr("Write %1 back onto the device %2?").arg(QFileInfo(path).fileName(), m_deviceMac),
        QMessageBox::Cancel, this);
    ask.get()->setInformativeText(
        tr("The device's flash becomes exactly what it was when the backup was made: its firmware, "
           "settings and data. Anything since is lost."));
    QPushButton* go = ask.get()->addButton(tr("Restore"), QMessageBox::DestructiveRole);
    ask.get()->exec();
    if (ask.get()->clickedButton() != go)
        return;
    m_jobDone = tr("The backup is restored and the device is restarting.");
    runJob(int(EspFirmwareJob::Action::Restore), {}, path);
}

void FirmwareInstallDialog::runJob(int action, const QString& backupPath, const QString& restoreFrom)
{
    EspFirmwareJob::Request r;
    r.portName = m_port;
    r.action = static_cast<EspFirmwareJob::Action>(action);
    r.manifest = m_manifest;
    r.files = m_files;
    r.backupPath = backupPath;
    r.restoreFrom = restoreFrom;
    r.requireCtr2MaxRegistration = true;   // this window installs AetherCTR2 on CTR2-Max units
    m_bar->setValue(0);
    m_step->setText(tr("Connecting…"));
    if (m_job->start(r))
        m_poll->stop();
    sync();
}

void FirmwareInstallDialog::onCancel()
{
    if (m_model && m_model->firmwareUpdateRunning())
        m_model->cancelFirmwareUpdate();
    m_job->cancel();
    m_step->setText(tr("Stopping…"));
}

void FirmwareInstallDialog::onProgress(const QString& step, qint64 done, qint64 total)
{
    m_step->setText(step);
    m_bar->setValue(total > 0 ? int(done * 100 / total) : 0);
}

void FirmwareInstallDialog::onFinished(bool ok, const QString& message)
{
    QString text = ok ? m_jobDone : message;
    m_jobDone.clear();
#ifdef Q_OS_LINUX
    if (!ok && message.contains(QStringLiteral("Permission")))
        text += QLatin1Char(' ')
                + tr("Add yourself to the group that owns the port (dialout or uucp, depending on "
                     "the distribution) and log in again.");
#endif
    m_step->setText(text);
    m_port.clear();
    m_poll->start(kPollMs);
    sync();
}

} // namespace AetherSDR
