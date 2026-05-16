#include "ProfileImportExportDialog.h"

#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCloseEvent>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

static const QString kDialogStyle =
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QTabWidget::pane { border: 1px solid #203040; background: #0f0f1a; }"
    "QTabBar::tab { background: #1a2a3a; color: #8898a8; padding: 6px 14px;"
    "  border: 1px solid #203040; border-bottom: none; border-top-left-radius: 4px;"
    "  border-top-right-radius: 4px; margin-right: 2px; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8; }"
    "QGroupBox { border: 1px solid #203040; border-radius: 4px; margin-top: 10px;"
    "  padding: 10px 8px 8px 8px; color: #c8d8e8; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QLineEdit { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 4px 6px; color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QPushButton:disabled { color: #607080; background: #101824; }"
    "QCheckBox { color: #c8d8e8; spacing: 8px; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }"
    "QProgressBar { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; text-align: center; }"
    "QProgressBar::chunk { background: #00b4d8; }";

QString profileTransferDirectory()
{
    auto& settings = AppSettings::instance();
    const QString saved = settings.value(QStringLiteral("ProfileImportExportPath"), QString()).toString();
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;

    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.isEmpty())
        return docs;
    return QDir::homePath();
}

void rememberProfileTransferDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (!info.absolutePath().isEmpty())
        AppSettings::instance().setValue(QStringLiteral("ProfileImportExportPath"), info.absolutePath());
}

QString sanitizedVersion(QString version)
{
    version = version.trimmed();
    version.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9_.-])")), QStringLiteral("_"));
    return version;
}

} // namespace

ProfileImportExportDialog::ProfileImportExportDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(QStringLiteral("Import/Export Profiles"),
                       QStringLiteral("ProfileImportExportDialogGeometry"), parent),
      m_model(model)
{
    setMinimumSize(560, 430);
    setStyleSheet(kDialogStyle);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(9);

    m_transfer = new ProfileTransfer(model, this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildExportPage(), QStringLiteral("Export"));
    m_tabs->addTab(buildImportPage(), QStringLiteral("Import"));
    root->addWidget(m_tabs, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("QLabel { color: #9fb0c0; }"));
    root->addWidget(m_statusLabel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelButton->setEnabled(false);
    m_closeButton = new QPushButton(QStringLiteral("Close"), this);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_closeButton);
    root->addLayout(buttonRow);

    connect(m_cancelButton, &QPushButton::clicked, m_transfer, &ProfileTransfer::cancel);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_transfer, &ProfileTransfer::started, this, [this](ProfileTransfer::Operation) {
        setTransferring(true);
    });
    connect(m_transfer, &ProfileTransfer::progress, this,
            [this](const QString& status, qint64 done, qint64 total) {
        setStatus(status);
        if (total > 0) {
            m_progress->setRange(0, 100);
            m_progress->setValue(static_cast<int>((done * 100) / total));
        } else {
            m_progress->setRange(0, 0);
        }
    });
    connect(m_transfer, &ProfileTransfer::finished, this,
            [this](ProfileTransfer::Operation operation, const QString& path) {
        setTransferring(false);
        m_progress->setRange(0, 100);
        m_progress->setValue(100);
        const QString message = operation == ProfileTransfer::Operation::ExportDatabase
            ? QStringLiteral("Export complete. The radio created the backup package and AetherSDR saved it.")
            : QStringLiteral("Import sent. The radio accepted the package and profile lists are being refreshed.");
        setStatus(message);
        rememberProfileTransferDirectory(path);
        QMessageBox::information(this, windowTitle(), message);
    });
    connect(m_transfer, &ProfileTransfer::failed, this,
            [this](ProfileTransfer::Operation, const QString& error) {
        setTransferring(false);
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        setStatus(error);
        QMessageBox::warning(this, windowTitle(), error);
    });

    if (m_model) {
        connect(m_model, &RadioModel::connectionStateChanged, this, [this](bool) { updateControls(); });
        connect(m_model, &RadioModel::radioTransmittingChanged, this, [this](bool) { updateControls(); });
        connect(m_model, &RadioModel::infoChanged, this, [this] {
            if (m_exportPath && m_exportPath->text().isEmpty())
                m_exportPath->setText(defaultExportPath());
            updateControls();
        });
        connect(&m_model->transmitModel(), &TransmitModel::moxChanged, this, [this](bool) { updateControls(); });
        connect(&m_model->transmitModel(), &TransmitModel::tuneChanged, this, [this](bool) { updateControls(); });
    }

    updateControls();
}

void ProfileImportExportDialog::closeEvent(QCloseEvent* event)
{
    if (m_transfer && m_transfer->isBusy()) {
        const auto choice = QMessageBox::question(
            this, windowTitle(),
            QStringLiteral("A profile transfer is still running. Cancel it and close this window?"));
        if (choice != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_transfer->cancel();
    }
    PersistentDialog::closeEvent(event);
}

QWidget* ProfileImportExportDialog::buildExportPage()
{
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setSpacing(10);

    auto* intro = new QLabel(
        QStringLiteral("Export is radio-driven: AetherSDR tells the radio which categories you selected, then saves the .ssdr_cfg backup package the radio creates. The package is not edited locally."),
        page);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("QLabel { color: #9fb0c0; }"));
    root->addWidget(intro);

    auto* options = new QGroupBox(QStringLiteral("Radio Database Categories"), page);
    auto* grid = new QGridLayout(options);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    m_selectAllExport = new QCheckBox(QStringLiteral("Select All"), options);
    m_globalProfiles = new QCheckBox(QStringLiteral("Global Profiles"), options);
    m_txProfiles = new QCheckBox(QStringLiteral("TX Profiles"), options);
    m_micProfiles = new QCheckBox(QStringLiteral("MIC Profiles"), options);
    m_memories = new QCheckBox(QStringLiteral("Memories"), options);
    m_preferences = new QCheckBox(QStringLiteral("Preferences"), options);
    m_tnf = new QCheckBox(QStringLiteral("TNF"), options);
    m_xvtr = new QCheckBox(QStringLiteral("XVTR"), options);
    m_usbCables = new QCheckBox(QStringLiteral("USB Cables"), options);

    m_globalProfiles->setChecked(true);
    m_txProfiles->setChecked(true);
    m_micProfiles->setChecked(true);

    grid->addWidget(m_selectAllExport, 0, 0, 1, 2);
    grid->addWidget(m_globalProfiles, 1, 0);
    grid->addWidget(m_txProfiles, 1, 1);
    grid->addWidget(m_micProfiles, 2, 0);
    grid->addWidget(m_memories, 2, 1);
    grid->addWidget(m_preferences, 3, 0);
    grid->addWidget(m_tnf, 3, 1);
    grid->addWidget(m_xvtr, 4, 0);
    grid->addWidget(m_usbCables, 4, 1);
    root->addWidget(options);

    auto* fileGroup = new QGroupBox(QStringLiteral("Destination"), page);
    auto* fileRow = new QHBoxLayout(fileGroup);
    m_exportPath = new QLineEdit(defaultExportPath(), fileGroup);
    auto* browse = new QPushButton(QStringLiteral("Browse..."), fileGroup);
    fileRow->addWidget(m_exportPath, 1);
    fileRow->addWidget(browse);
    root->addWidget(fileGroup);

    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch();
    m_exportButton = new QPushButton(QStringLiteral("Export"), page);
    actionRow->addWidget(m_exportButton);
    root->addLayout(actionRow);
    root->addStretch();

    const auto optionChanged = [this] {
        updateSelectAllFromOptions();
        updateControls();
    };
    for (QCheckBox* box : {m_globalProfiles, m_txProfiles, m_micProfiles, m_memories,
                           m_preferences, m_tnf, m_xvtr, m_usbCables}) {
        connect(box, &QCheckBox::toggled, this, optionChanged);
    }
    connect(m_selectAllExport, &QCheckBox::toggled, this, [this](bool checked) {
        const QSignalBlocker blocker(m_selectAllExport);
        for (QCheckBox* box : {m_globalProfiles, m_txProfiles, m_micProfiles, m_memories,
                               m_preferences, m_tnf, m_xvtr, m_usbCables}) {
            box->setChecked(checked);
        }
        updateControls();
    });
    connect(browse, &QPushButton::clicked, this, &ProfileImportExportDialog::chooseExportPath);
    connect(m_exportPath, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(m_exportButton, &QPushButton::clicked, this, &ProfileImportExportDialog::startExport);

    updateSelectAllFromOptions();
    return page;
}

QWidget* ProfileImportExportDialog::buildImportPage()
{
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setSpacing(10);

    auto* intro = new QLabel(
        QStringLiteral("Import is radio-driven too: AetherSDR uploads the .ssdr_cfg package, then the radio unpacks it and applies the database contents."),
        page);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("QLabel { color: #9fb0c0; }"));
    root->addWidget(intro);

    auto* fileGroup = new QGroupBox(QStringLiteral("Configuration Package"), page);
    auto* fileRow = new QHBoxLayout(fileGroup);
    m_importPath = new QLineEdit(fileGroup);
    auto* browse = new QPushButton(QStringLiteral("Browse..."), fileGroup);
    fileRow->addWidget(m_importPath, 1);
    fileRow->addWidget(browse);
    root->addWidget(fileGroup);

    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch();
    m_importButton = new QPushButton(QStringLiteral("Import"), page);
    actionRow->addWidget(m_importButton);
    root->addLayout(actionRow);
    root->addStretch();

    connect(browse, &QPushButton::clicked, this, &ProfileImportExportDialog::chooseImportPath);
    connect(m_importPath, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(m_importButton, &QPushButton::clicked, this, &ProfileImportExportDialog::startImport);
    return page;
}

ExportSelection ProfileImportExportDialog::currentExportSelection() const
{
    ExportSelection selection;
    selection.globalProfiles = m_globalProfiles && m_globalProfiles->isChecked();
    selection.txProfiles = m_txProfiles && m_txProfiles->isChecked();
    selection.micProfiles = m_micProfiles && m_micProfiles->isChecked();
    selection.memories = m_memories && m_memories->isChecked();
    selection.preferences = m_preferences && m_preferences->isChecked();
    selection.tnf = m_tnf && m_tnf->isChecked();
    selection.xvtr = m_xvtr && m_xvtr->isChecked();
    selection.usbCables = m_usbCables && m_usbCables->isChecked();
    return selection;
}

QString ProfileImportExportDialog::defaultExportPath() const
{
    return QDir(profileTransferDirectory()).filePath(defaultExportFileName());
}

QString ProfileImportExportDialog::defaultExportFileName() const
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString version = m_model ? sanitizedVersion(m_model->softwareVersion()) : QString();
    if (version.isEmpty())
        return QStringLiteral("ASDR_Config_%1.ssdr_cfg").arg(timestamp);
    return QStringLiteral("ASDR_Config_%1_v%2.ssdr_cfg").arg(timestamp, version);
}

bool ProfileImportExportDialog::canTransfer(QString* reason) const
{
    if (!m_model || !m_model->isConnected()) {
        if (reason)
            *reason = QStringLiteral("Connect to a FlexRadio before importing or exporting profiles.");
        return false;
    }
    if (m_model->isWan()) {
        if (reason)
            *reason = QStringLiteral("SmartLink/WAN database transfer is disabled. Use a direct LAN connection.");
        return false;
    }
    if (m_model->isProfileTransferBlocked()) {
        if (reason)
            *reason = QStringLiteral("Stop MOX, TUNE, PTT, or active transmit before importing or exporting.");
        return false;
    }
    if (reason)
        reason->clear();
    return true;
}

bool ProfileImportExportDialog::confirmImport()
{
    QString versionLine;
    const QFileInfo info(m_importPath->text());
    const QVersionNumber exportVersion = parseSmartSdrVersionFromFilename(info.fileName());
    const QString radioVersion = m_model ? m_model->softwareVersion() : QString();
    if (exportVersion.isNull()) {
        versionLine = QStringLiteral("The package firmware version cannot be verified from the filename.");
    } else if (!radioVersion.trimmed().isEmpty()
               && compareFirmwareVersions(exportVersion.toString(), radioVersion) > 0) {
        versionLine = QStringLiteral("This package appears to be from newer firmware (%1) than the connected radio (%2).")
            .arg(exportVersion.toString(), radioVersion);
    } else if (!radioVersion.trimmed().isEmpty()) {
        versionLine = QStringLiteral("Package firmware version: %1. Connected radio firmware: %2.")
            .arg(exportVersion.toString(), radioVersion);
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Confirm Profile Import"));
    box.setText(QStringLiteral("Import this .ssdr_cfg backup to the radio?"));
    box.setInformativeText(
        QStringLiteral("AetherSDR will upload the backup package, but the radio does the actual import. AetherSDR does not rewrite the database inside the package.\n\n"
                       "Profiles, memories, and other included settings can replace matching items already on the radio, including defaults.\n"
                       "Packages from newer firmware may fail or leave the radio database in an unexpected state.\n"
                       "Packages that include Preferences can close or reopen slices, panadapters, and other saved radio state.\n\n"
                       "Export a backup first if you have not already done so.\n%1")
            .arg(versionLine));
    auto* importButton = box.addButton(QStringLiteral("Import"), QMessageBox::DestructiveRole);
    auto* backupButton = canTransfer() && m_transfer && !m_transfer->isBusy()
        ? box.addButton(QStringLiteral("Export Backup First"), QMessageBox::ActionRole)
        : nullptr;
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == backupButton) {
        m_tabs->setCurrentIndex(0);
        startExport();
        return false;
    }
    return box.clickedButton() == importButton;
}

void ProfileImportExportDialog::chooseExportPath()
{
    QString initial = m_exportPath ? m_exportPath->text() : defaultExportPath();
    if (initial.trimmed().isEmpty())
        initial = defaultExportPath();
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Profile Backup"),
        initial, QStringLiteral("Profile Backup (*.ssdr_cfg)"));
    if (path.isEmpty())
        return;
    m_exportPath->setText(path.endsWith(QStringLiteral(".ssdr_cfg"), Qt::CaseInsensitive)
                              ? path
                              : path + QStringLiteral(".ssdr_cfg"));
}

void ProfileImportExportDialog::chooseImportPath()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Profile Backup"),
        profileTransferDirectory(), QStringLiteral("Profile Backup (*.ssdr_cfg)"));
    if (path.isEmpty())
        return;
    m_importPath->setText(path);
}

void ProfileImportExportDialog::startExport()
{
    if (!m_exportPath || m_exportPath->text().trimmed().isEmpty())
        m_exportPath->setText(defaultExportPath());
    rememberProfileTransferDirectory(m_exportPath->text());
    m_transfer->exportDatabase(currentExportSelection(), m_exportPath->text());
}

void ProfileImportExportDialog::startImport()
{
    if (!m_importPath || m_importPath->text().trimmed().isEmpty()) {
        chooseImportPath();
        if (!m_importPath || m_importPath->text().trimmed().isEmpty())
            return;
    }
    rememberProfileTransferDirectory(m_importPath->text());
    if (!confirmImport())
        return;
    m_transfer->importDatabase(m_importPath->text());
}

void ProfileImportExportDialog::setTransferring(bool transferring)
{
    const bool controlsEnabled = !transferring;
    for (QCheckBox* box : {m_selectAllExport, m_globalProfiles, m_txProfiles, m_micProfiles,
                           m_memories, m_preferences, m_tnf, m_xvtr, m_usbCables}) {
        if (box)
            box->setEnabled(controlsEnabled);
    }
    if (m_exportPath)
        m_exportPath->setEnabled(controlsEnabled);
    if (m_importPath)
        m_importPath->setEnabled(controlsEnabled);
    if (m_cancelButton)
        m_cancelButton->setEnabled(transferring);
    if (m_closeButton)
        m_closeButton->setEnabled(!transferring);
    updateControls();
}

void ProfileImportExportDialog::updateControls()
{
    QString reason;
    const bool transferable = canTransfer(&reason);
    const bool busy = m_transfer && m_transfer->isBusy();
    const bool exportHasPath = m_exportPath && !m_exportPath->text().trimmed().isEmpty();
    const bool importHasPath = m_importPath && !m_importPath->text().trimmed().isEmpty();
    const bool exportHasSelection = currentExportSelection().anySelected();

    if (m_exportButton)
        m_exportButton->setEnabled(!busy && transferable && exportHasPath && exportHasSelection);
    if (m_importButton)
        m_importButton->setEnabled(!busy && transferable && importHasPath);

    if (busy)
        return;

    if (!reason.isEmpty())
        setStatus(reason);
    else
        setStatus(QStringLiteral("Ready. Import and export are radio-driven; AetherSDR handles the .ssdr_cfg transfer."));
}

void ProfileImportExportDialog::updateSelectAllFromOptions()
{
    if (!m_selectAllExport)
        return;
    const bool allSelected =
        m_globalProfiles->isChecked() && m_txProfiles->isChecked() && m_micProfiles->isChecked()
        && m_memories->isChecked() && m_preferences->isChecked() && m_tnf->isChecked()
        && m_xvtr->isChecked() && m_usbCables->isChecked();
    const QSignalBlocker blocker(m_selectAllExport);
    m_selectAllExport->setChecked(allSelected);
}

void ProfileImportExportDialog::setStatus(const QString& text)
{
    if (m_statusLabel)
        m_statusLabel->setText(text);
}

} // namespace AetherSDR
