#include "FirmwareInstallDialog.h"

#include "CanonIndicators.h"
#include "ComboStyle.h"
#include "Ctr2ProxyPanel.h"
#include "ModemChrome.h"
#include "ScopedChildWidget.h"
#include "models/Ctr2ProxyModel.h"
#include "models/FirmwareInstallModel.h"

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
#include <QStyle>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

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
    : CanonWindow(tr("CTR2"), parent, Kind::Workspace)
    , m_model(model)
    , m_fw(new FirmwareInstallModel(model, this))
{
    setObjectName(QStringLiteral("firmwareInstallDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometryKey(QStringLiteral("Ctr2WindowGeometry"));
    resize(540, 800);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
               + QStringLiteral(
                   "QListWidget#firmwareInstallProduct { background: {{color.canon.nested}};"
                   " color: {{color.canon.ink}}; border: 1px solid {{color.canon.line}};"
                   " border-radius: 6px; padding: 4px; outline: 0; }"
                   "QListWidget#firmwareInstallProduct::item { padding: 5px 8px; border-radius: 4px; }"
                   "QListWidget#firmwareInstallProduct::item:hover { background: {{color.canon.raised}}; }"
                   "QListWidget#firmwareInstallProduct::item:selected {"
                   " background: {{color.canon.control}}; color: {{color.canon.cyan}}; }"
                   // The status badge; showStatus() picks its tone.
                   "QLabel#firmwareInstallStatus { background: {{color.canon.nested}};"
                   " border: 1px solid {{color.canon.muted}}; color: {{color.canon.muted}};"
                   " border-radius: 11px; padding: 3px 12px; font-weight: 600; }"
                   "QLabel#firmwareInstallStatus[tone=\"cyan\"] { color: {{color.canon.cyan}};"
                   " border-color: {{color.canon.cyan}}; }"
                   "QLabel#firmwareInstallStatus[tone=\"sparkGold\"] {"
                   " color: {{color.canon.sparkGold}}; border-color: {{color.canon.sparkGold}}; }"
                   "QLabel#firmwareInstallStatus[tone=\"aqua\"] { color: {{color.canon.aqua}};"
                   " border-color: {{color.canon.aqua}}; }"
                   // The relay panel (Ctr2ProxyPanel): its combos and value tones.
                   "QComboBox { color: {{color.canon.ink}}; background: {{color.canon.control}};"
                   " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 4px 8px; }"
                   "QComboBox:hover, QComboBox:focus { border-color: {{color.canon.aqua}}; }"
                   "QComboBox:disabled { color: {{color.canon.muted}};"
                   " border-color: {{color.canon.line}}; }"
                   "QComboBox::drop-down { border: 0; width: 18px; }"
                   "QComboBox QAbstractItemView { color: {{color.canon.inkSoft}};"
                   " background: {{color.canon.nested}}; border: 1px solid {{color.canon.lineHi}};"
                   " selection-background-color: {{color.canon.cyan}};"
                   " selection-color: {{color.canon.onAccent}}; }"
                   "QLabel[tone=\"secondary\"] { color: {{color.canon.inkSoft}}; }"
                   "QLabel[tone=\"readout\"] { color: {{color.canon.ink}};"
                   " font-family: \"SF Mono\", \"Menlo\", \"Consolas\", monospace; }"
                   "QLabel[tone=\"warning\"] { color: {{color.canon.sparkGold}}; }"
                   "QLabel[tone=\"danger\"] { color: {{color.accent.danger}}; }")
               + comboArrowRule();
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    // The relay first: it is what the window is opened for day to day.
    root->addWidget(makeCanonHeader(tr("CTR2 Relay")));
    m_relay = new Ctr2ProxyPanel;
    m_relay->setModel(model);
    root->addWidget(m_relay);
    root->addSpacing(6);

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

    // A CTR2-Max running AetherCTR2 restarts on request: into its stock
    // firmware, or into download mode for the actions above.
    auto* restartRow = new QHBoxLayout;
    restartRow->setSpacing(6);
    auto* restartLabel = new QLabel(tr("Restart into"));
    restartLabel->setObjectName(QStringLiteral("SectionLabel"));
    restartRow->addWidget(restartLabel);
    m_toStock = new QPushButton(tr("Stock Firmware"));
    m_toStock->setObjectName(QStringLiteral("firmwareInstallToStock"));
    m_toStock->setToolTip(tr("Restart the CTR2-Max into its stock firmware. AetherCTR2 stays "
                             "installed; to come back, choose Run AetherCTR2 in the CTR2-Max's "
                             "Settings menu (v1.02.00 and later), or put it in download mode "
                             "and press Start."));
    restartRow->addWidget(m_toStock);
    m_toBootloader = new QPushButton(tr("Bootloader"));
    m_toBootloader->setObjectName(QStringLiteral("firmwareInstallToBootloader"));
    m_toBootloader->setToolTip(tr("Restart the CTR2-Max into download mode, without the BOOT and "
                                  "RESET buttons, to back it up, restore it or install."));
    restartRow->addWidget(m_toBootloader);
    restartRow->addStretch(1);
    root->addLayout(restartRow);

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

    connect(m_product, &QListWidget::currentRowChanged, m_fw, &FirmwareInstallModel::chooseRelease);
    connect(m_install, &QPushButton::clicked, this, &FirmwareInstallDialog::onInstall);
    connect(m_start, &QPushButton::clicked, this, &FirmwareInstallDialog::onStart);
    connect(m_backup, &QPushButton::clicked, this, &FirmwareInstallDialog::onBackup);
    connect(m_restore, &QPushButton::clicked, this, &FirmwareInstallDialog::onRestore);
    connect(m_toStock, &QPushButton::clicked, this, [this] { restartDevice(false); });
    connect(m_toBootloader, &QPushButton::clicked, this, [this] { restartDevice(true); });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_fw->cancel();
        m_step->setText(tr("Stopping…"));
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(m_fw, &FirmwareInstallModel::releasesChanged, this, &FirmwareInstallDialog::syncReleases);
    connect(m_fw, &FirmwareInstallModel::releaseTextChanged, this, [this] {
        m_release->setText(m_fw->releaseText());
        sync();
    });
    connect(m_fw, &FirmwareInstallModel::deviceChanged, this, &FirmwareInstallDialog::syncDevice);
    connect(m_fw, &FirmwareInstallModel::busyChanged, this, &FirmwareInstallDialog::sync);
    connect(m_fw, &FirmwareInstallModel::connected, this,
            [this](const QString& chip, const QByteArray& mac, quint32 flashSize) {
                showStatus(tr("Bootloader"), "aqua",
                           tr("Connected: %1, %2 MB flash, %3")
                               .arg(chip)
                               .arg(flashSize >> 20)
                               .arg(QString::fromLatin1(mac.toHex(':')).toUpper()));
            });
    connect(m_fw, &FirmwareInstallModel::stepChanged, m_step, &QLabel::setText);
    connect(m_fw, &FirmwareInstallModel::progress, this,
            [this](const QString& step, qint64 done, qint64 total) {
                if (!step.isEmpty())
                    m_step->setText(step);
                m_bar->setValue(total > 0 ? int(done * 100 / total) : 0);
            });
    connect(m_fw, &FirmwareInstallModel::finished, this, &FirmwareInstallDialog::onFinished);
    // The relay's version readout decides whether a link update is offered.
    if (m_model)
        connect(m_model, &Ctr2ProxyModel::firmwareVersionChanged, this,
                &FirmwareInstallDialog::syncDevice);

    syncDevice();
    m_fw->begin();
}

FirmwareInstallDialog::~FirmwareInstallDialog() = default;

void FirmwareInstallDialog::closeEvent(QCloseEvent* event)
{
    if (m_fw->busy()) {
        m_step->setText(tr("Wait for this step to finish, or press Cancel, before closing."));
        event->ignore();
        return;
    }
    CanonWindow::closeEvent(event);
}

void FirmwareInstallDialog::reject()
{
    if (m_fw->busy()) {
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

void FirmwareInstallDialog::syncReleases()
{
    if (m_fw->releases().isEmpty()) {
        showPlaceholder(m_fw->catalogProblem().isEmpty() ? tr("Looking for firmware…")
                                                         : m_fw->catalogProblem());
    } else {
        const QSignalBlocker block(m_product);
        m_product->clear();
        m_product->addItems(m_fw->releases());
    }
    sync();
}

void FirmwareInstallDialog::showStatus(const QString& state, const char* colorToken,
                                       const QString& detail)
{
    // The tone selects a rule in the dialog's sheet (cyan, sparkGold, aqua,
    // or the muted default).
    const QString tone = QString::fromLatin1(colorToken);
    if (m_badge->property("tone").toString() != tone) {
        m_badge->setProperty("tone", tone);
        m_badge->style()->unpolish(m_badge);
        m_badge->style()->polish(m_badge);
    }
    m_badge->setText(state);
    m_badge->setToolTip(detail);
    m_badge->setAccessibleDescription(detail);
}

void FirmwareInstallDialog::syncDevice()
{
    const QString mac = m_fw->deviceMac();
    switch (m_fw->device()) {
    case FirmwareInstallModel::Device::Bootloader: {
        QString detail = tr("%1. Its screen stays dark until it restarts; that is normal.").arg(mac);
        if (m_fw->loaderCount() > 1)
            detail += QLatin1Char(' ')
                      + tr("%1 devices are in download mode; connect only the one to install.")
                            .arg(m_fw->loaderCount());
        showStatus(tr("Bootloader"), "aqua", detail);
        break;
    }
    case FirmwareInstallModel::Device::LinkFirmware: {
        const QString version = m_fw->linkVersion();
        QString detail = mac.isEmpty() ? QString() : mac + QStringLiteral(". ");
        detail += version.isEmpty() ? tr("Put it in download mode to install.")
                                    : tr("Running %1; Install updates it over the USB cable.")
                                          .arg(version);
        showStatus(m_fw->deviceName(), "cyan", detail);
        break;
    }
    case FirmwareInstallModel::Device::Stock:
        showStatus(tr("Stock firmware"), "sparkGold",
                   (mac.isEmpty() ? QString() : mac + QStringLiteral(". "))
                       + tr("Put it in download mode to install."));
        break;
    case FirmwareInstallModel::Device::None:
        showStatus(tr("No CTR2-Max"), "muted", tr("Connect the CTR2-Max with a USB cable."));
        break;
    }
    sync();
}

namespace {

// Enables a button, and when it is not, says why to assistive technology.
void offer(QPushButton* b, bool enabled, const QString& why)
{
    b->setEnabled(enabled);
    b->setAccessibleDescription(enabled ? QString() : why);
}

} // namespace

void FirmwareInstallDialog::sync()
{
    using Device = FirmwareInstallModel::Device;
    const bool running = m_fw->busy();
    const Device device = m_fw->device();
    const bool loader = device == Device::Bootloader;
    // Over the link: firmware that updates itself and reports its version.
    const bool link = device == Device::LinkFirmware && !m_fw->linkVersion().isEmpty();
    const bool release = m_fw->haveRelease();
    const QString busy = tr("Unavailable while the current step runs.");
    const QString noDevice = device == Device::None ? tr("No CTR2-Max is connected.")
                                                    : tr("Put the CTR2-Max in download mode first.");
    const QString noRelease = tr("Choose a release first.");
    offer(m_install, !running && (loader || link) && release,
          running ? busy : !release ? noRelease : noDevice);
    offer(m_start, !running && loader && release,
          running ? busy : !release ? noRelease : noDevice);
    offer(m_backup, !running && loader, running ? busy : noDevice);
    offer(m_restore, !running && loader, running ? busy : noDevice);
    const QString notAetherCtr2 = tr("The CTR2-Max is not running AetherCTR2.");
    offer(m_toStock, !running && link, running ? busy : notAetherCtr2);
    offer(m_toBootloader, !running && link, running ? busy : notAetherCtr2);
    m_product->setEnabled(!running);
    m_cancel->setVisible(running);
    m_bar->setVisible(running);
}

QString FirmwareInstallDialog::suggestBackupPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString mac = QString(m_fw->deviceMac()).remove(QLatin1Char(':'));
    return QDir(dir).filePath(QStringLiteral("flash-backup-%1-%2.bin")
                                  .arg(mac, QDate::currentDate().toString(Qt::ISODate)));
}

void FirmwareInstallDialog::started(bool ok, const QString& problem, const QString& doneText)
{
    if (!ok) {
        m_jobDone.clear();
        m_step->setText(problem);
        sync();
        return;
    }
    // An action that finished at once (a device that could not be opened)
    // has already reported through onFinished().
    if (m_fw->busy()) {
        m_jobDone = doneText;
        m_bar->setValue(0);
    }
    sync();
}

void FirmwareInstallDialog::onInstall()
{
    // The device is the one shown now; the model refuses if it changes
    // while the questions below are open.
    const FirmwareInstallModel::Target target = m_fw->target();
    if (target.device == FirmwareInstallModel::Device::LinkFirmware) {
        installOverLink();
        return;
    }
    const QString product = m_fw->releaseProduct();
    const QString version = m_fw->releaseVersion();
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Question, tr("Install %1?").arg(product),
        tr("Install %1 %2 on the CTR2-Max %3?").arg(product, version, target.mac),
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
    QString done;
    if (ask.get()->clickedButton() == withBackup) {
        path = QFileDialog::getSaveFileName(this, tr("Save the backup"), suggestBackupPath(),
                                            tr("Flash backup (*.bin)"));
        if (path.isEmpty())
            return;
        done = tr("%1 %2 is installed and starting. The backup is in %3.").arg(product, version, path);
    } else if (ask.get()->clickedButton() == without) {
        done = tr("%1 %2 is installed and starting.").arg(product, version);
    } else {
        return;
    }
    m_step->setText(tr("Connecting…"));
    QString problem;
    m_jobDone = done;
    const bool ok = m_fw->install(target, path, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::installOverLink()
{
    const FirmwareInstallModel::Target target = m_fw->target();
    QString problem;
    if (!m_fw->canInstallOverLink(&problem)) {
        m_step->setText(problem);
        return;
    }
    const QString product = m_fw->releaseProduct();
    const QString version = m_fw->releaseVersion();
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Question, tr("Install %1?").arg(product),
        tr("Install %1 %2 over the USB cable? The device runs %3 now.")
            .arg(product, version, m_fw->linkVersion()),
        QMessageBox::Cancel, this);
    ask.get()->setInformativeText(
        tr("It takes about three minutes. The device checks the firmware's signature, and keeps "
           "the firmware it has if anything goes wrong."));
    QPushButton* go = ask.get()->addButton(tr("Install"), QMessageBox::AcceptRole);
    ask.get()->setDefaultButton(go);
    ask.get()->exec();
    if (ask.get()->clickedButton() != go)
        return;
    const QString done = tr("%1 %2 is installed and running.").arg(product, version);
    m_step->setText(tr("Starting the update…"));
    m_jobDone = done;
    const bool ok = m_fw->installOverLink(target, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::restartDevice(bool bootloader)
{
    const QString done =
        bootloader ? tr("The CTR2-Max is restarting into download mode.")
                   : tr("The CTR2-Max is restarting into its stock firmware. To come back to "
                        "AetherCTR2, choose Run AetherCTR2 in its Settings menu, or put it in "
                        "download mode and press Start.");
    m_step->setText(tr("Asking the CTR2-Max to restart…"));
    m_jobDone = done;
    QString problem;
    const bool ok = m_fw->restart(m_fw->target(), bootloader, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::onStart()
{
    const FirmwareInstallModel::Target target = m_fw->target();
    const QString done = tr("The device is starting %1.").arg(m_fw->releaseProduct());
    m_step->setText(tr("Connecting…"));
    m_jobDone = done;
    QString problem;
    const bool ok = m_fw->startFirmware(target, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::onBackup()
{
    const FirmwareInstallModel::Target target = m_fw->target();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the backup"), suggestBackupPath(),
                                                      tr("Flash backup (*.bin)"));
    if (path.isEmpty())
        return;
    const QString done = tr("Backed up to %1. The device is restarting.").arg(path);
    m_step->setText(tr("Connecting…"));
    m_jobDone = done;
    QString problem;
    const bool ok = m_fw->backup(target, path, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::onRestore()
{
    const FirmwareInstallModel::Target target = m_fw->target();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a backup"), suggestBackupPath(),
                                                      tr("Flash backup (*.bin)"));
    if (path.isEmpty())
        return;
    ScopedChildWidget<QMessageBox> ask(
        QMessageBox::Warning, tr("Restore the backup?"),
        tr("Write %1 back onto the device %2?").arg(QFileInfo(path).fileName(), target.mac),
        QMessageBox::Cancel, this);
    ask.get()->setInformativeText(
        tr("The device's flash becomes exactly what it was when the backup was made: its firmware, "
           "settings and data. Anything since is lost."));
    QPushButton* go = ask.get()->addButton(tr("Restore"), QMessageBox::DestructiveRole);
    ask.get()->exec();
    if (ask.get()->clickedButton() != go)
        return;
    const QString done = tr("The backup is restored and the device is restarting.");
    m_step->setText(tr("Connecting…"));
    m_jobDone = done;
    QString problem;
    const bool ok = m_fw->restore(target, path, &problem);
    started(ok, problem, done);
}

void FirmwareInstallDialog::onFinished(bool ok, const QString& message)
{
    QString text = ok ? m_jobDone : message;
    // Done, but the device did not restart by itself: say so, with what to do.
    if (ok && !message.isEmpty())
        text += QLatin1Char(' ')
                + tr("It did not restart by itself (%1): press RESET on the CTR2-Max.").arg(message);
    m_jobDone.clear();
#ifdef Q_OS_LINUX
    if (!ok && message.contains(QStringLiteral("Permission")))
        text += QLatin1Char(' ')
                + tr("Add yourself to the group that owns the port (dialout or uucp, depending on "
                     "the distribution) and log in again.");
#endif
    m_step->setText(text);
    sync();
}

} // namespace AetherSDR
