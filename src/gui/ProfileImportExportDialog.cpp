#include "ProfileImportExportDialog.h"

#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "ScopedChildWidget.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// What the shared canon dialog sheet (ModemChrome::Look::Canon: fields,
// buttons, the canon check box, the progress bar) leaves out: tabs and group
// boxes as MQTT Settings has them; selection ink on the path fields (#6917);
// the intro and status lines; and the path fields' right-click menus,
// children of the fields that the sheet's transparent rule would otherwise
// leave with no panel.
constexpr const char* kProfileIoRules = R"(
QTabWidget::pane {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 10px;
    top: -1px;
}
QTabBar::tab {
    color: {{color.canon.muted}};
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 6px 12px;
    font-weight: 600;
}
QTabBar::tab:hover {
    color: {{color.canon.ink}};
}
QTabBar::tab:selected {
    color: {{color.canon.cyan}};
    border-bottom-color: {{color.canon.cyan}};
}
QGroupBox {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    margin-top: 14px;
    padding: 10px 8px 8px 8px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: {{color.canon.cyan}};
}
QLineEdit {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QLabel#profileIoIntro {
    color: {{color.canon.muted}};
}
QLabel#profileIoStatus {
    color: {{color.canon.inkSoft}};
}
QMenu {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.lineHi}};
}
QMenu::item {
    padding: 4px 18px;
}
QMenu::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QMenu::item:disabled {
    color: {{color.control.unavailable}};
}
QMenu::separator {
    height: 1px;
    margin: 4px 8px;
    background: {{color.canon.line}};
}
)";

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
    : CanonWindow(QStringLiteral("Import/Export Profiles"), parent, Kind::Workspace),
      m_model(model)
{
    setObjectName(QStringLiteral("ProfileImportExportDialog"));
    theme::setContainer(this, QStringLiteral("canon/profileIO"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("ProfileImportExportDialogGeometry"));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
             + QString::fromLatin1(kProfileIoRules);
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(9);
    root->addWidget(makeCanonHeader(QStringLiteral("Import/Export Profiles")));

    m_transfer = new ProfileTransfer(model, this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildExportPage(), QStringLiteral("Export"));
    m_tabs->addTab(buildImportPage(), QStringLiteral("Import"));
    root->addWidget(m_tabs, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("profileIoStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextFormat(Qt::PlainText);
    root->addWidget(m_statusLabel);

    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(QStringLiteral("Transfer progress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelButton->setObjectName(QStringLiteral("profileIoCancel"));
    m_cancelButton->setAccessibleName(QStringLiteral("Cancel transfer"));
    m_cancelButton->setEnabled(false);
    buttonRow->addWidget(m_cancelButton);
    root->addLayout(buttonRow);

    // A lower bound until showEvent() measures the polished layout.
    setMinimumSize(QSize(560, 470).expandedTo(minimumSizeHint()));

    connect(m_cancelButton, &QPushButton::clicked, m_transfer, &ProfileTransfer::cancel);

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
        ScopedChildWidget<CanonMessageBox> boxOwner(
            operation == ProfileTransfer::Operation::ExportDatabase
                ? QStringLiteral("Export Complete") : QStringLiteral("Import Sent"),
            message, this);
        CanonMessageBox& box = *boxOwner.get();
        box.setDefaultButton(box.addButton(QStringLiteral("OK"), CanonMessageBox::Role::Accept));
        box.exec();
    });
    connect(m_transfer, &ProfileTransfer::failed, this,
            [this](ProfileTransfer::Operation operation, const QString& error) {
        setTransferring(false);
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        setStatus(error);
        // The operator just chose Cancel Transfer to close the window; a
        // notice that it stopped would only stand in the way of the close.
        if (m_closingAfterCancel)
            return;
        // A canon box has no warning icon, so the title says what happened;
        // the text says why (a failure, or the operator's own Cancel).
        ScopedChildWidget<CanonMessageBox> boxOwner(
            operation == ProfileTransfer::Operation::ExportDatabase
                ? QStringLiteral("Export Did Not Complete")
                : QStringLiteral("Import Did Not Complete"),
            error, this);
        CanonMessageBox& box = *boxOwner.get();
        box.setDefaultButton(box.addButton(QStringLiteral("OK"), CanonMessageBox::Role::Accept));
        box.exec();
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

void ProfileImportExportDialog::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    // Measure the polished layout at its minimum width, including wrapped
    // intros that a constructor-time minimumSizeHint() cannot account for.
    if (QLayout* l = layout()) {
        l->activate();
        const int width = qMax(minimumWidth(), l->totalMinimumSize().width());
        const int needed = l->hasHeightForWidth() ? l->totalHeightForWidth(width)
                                                  : l->totalMinimumSize().height();
        setMinimumSize(QSize(width, qMax(minimumHeight(), needed)));
    }
}

void ProfileImportExportDialog::done(int result)
{
    if (m_confirmingClose)
        return;                        // the question is already up
    if (m_transfer && m_transfer->isBusy()) {
        const QPointer<ProfileImportExportDialog> self(this);
        const QPointer<ProfileTransfer> transferGuard(m_transfer);
        m_confirmingClose = true;
        // Keep Transferring is the default: Return, Esc or the ✕ on the
        // question leave the transfer running and the window open.
        ScopedChildWidget<CanonMessageBox> boxOwner(
            windowTitle(),
            QStringLiteral("A profile transfer is still running. Cancel it and close this window?"),
            this);
        CanonMessageBox& box = *boxOwner.get();
        auto* keep = box.addButton(QStringLiteral("Keep Transferring"), CanonMessageBox::Role::Reject);
        auto* stop = box.addButton(QStringLiteral("Cancel Transfer"), CanonMessageBox::Role::Destructive);
        box.setDefaultButton(keep);
        box.exec();
        if (!self)
            return;
        self->m_confirmingClose = false;
        if (!boxOwner || box.clickedButton() != stop)
            return;
        // The transfer may have ended while the question was up.
        if (transferGuard && self->m_transfer == transferGuard.data() && transferGuard->isBusy()) {
            self->m_closingAfterCancel = true;
            transferGuard->cancel();      // emits failed() synchronously
            if (!self) {
                return;
            }
            self->m_closingAfterCancel = false;
        }
    }
    CanonWindow::done(result);
}

QWidget* ProfileImportExportDialog::buildExportPage()
{
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setSpacing(10);

    auto* intro = new QLabel(
        QStringLiteral("Export is radio-driven: AetherSDR tells the radio which categories you selected, then saves the .ssdr_cfg backup package the radio creates. The package is not edited locally."),
        page);
    intro->setObjectName(QStringLiteral("profileIoIntro"));
    intro->setWordWrap(true);
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
    m_exportPath->setObjectName(QStringLiteral("profileIoExportPath"));
    m_exportPath->setAccessibleName(QStringLiteral("Export destination path"));
    auto* browse = new QPushButton(QStringLiteral("Browse..."), fileGroup);
    browse->setAccessibleName(QStringLiteral("Browse for export destination"));
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
    intro->setObjectName(QStringLiteral("profileIoIntro"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* fileGroup = new QGroupBox(QStringLiteral("Configuration Package"), page);
    auto* fileRow = new QHBoxLayout(fileGroup);
    m_importPath = new QLineEdit(fileGroup);
    m_importPath->setObjectName(QStringLiteral("profileIoImportPath"));
    m_importPath->setAccessibleName(QStringLiteral("Import package path"));
    auto* browse = new QPushButton(QStringLiteral("Browse..."), fileGroup);
    browse->setAccessibleName(QStringLiteral("Browse for import package"));
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

    const QPointer<ProfileImportExportDialog> self(this);
    const QPointer<RadioModel> modelGuard(m_model);
    ScopedChildWidget<CanonMessageBox> boxOwner(
        QStringLiteral("Confirm Profile Import"),
        QStringLiteral("Import this .ssdr_cfg backup to the radio?"), this);
    CanonMessageBox& box = *boxOwner.get();
    box.setInformativeText(
        QStringLiteral("AetherSDR will upload the backup package, but the radio does the actual import. AetherSDR does not rewrite the database inside the package.\n\n"
                       "Profiles, memories, and other included settings can replace matching items already on the radio, including defaults.\n"
                       "Packages from newer firmware may fail or leave the radio database in an unexpected state.\n"
                       "Packages that include Preferences can close or reopen slices, panadapters, and other saved radio state.\n\n"
                       "Export a backup first if you have not already done so.\n%1")
            .arg(versionLine));
    // Cancel is the default: Return, Esc or the ✕ import nothing.
    auto* cancelButton = box.addButton(QStringLiteral("Cancel"), CanonMessageBox::Role::Reject);
    auto* backupButton = canTransfer() && m_transfer && !m_transfer->isBusy()
        ? box.addButton(QStringLiteral("Export Backup First"), CanonMessageBox::Role::Accept)
        : nullptr;
    auto* importButton = box.addButton(QStringLiteral("Import"), CanonMessageBox::Role::Destructive);
    box.setDefaultButton(cancelButton);
    box.exec();

    if (!self || !modelGuard || self->m_model != modelGuard.data() || !boxOwner) {
        return false;
    }

    if (backupButton && box.clickedButton() == backupButton) {
        m_tabs->setCurrentIndex(0);
        startExport();
        return false;
    }
    return box.clickedButton() == importButton;
}

void ProfileImportExportDialog::chooseExportPath()
{
    const QPointer<ProfileImportExportDialog> self(this);
    const QPointer<QLineEdit> exportPath(m_exportPath);
    QString initial = m_exportPath ? m_exportPath->text() : defaultExportPath();
    if (initial.trimmed().isEmpty())
        initial = defaultExportPath();
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Profile Backup"),
        initial, QStringLiteral("Profile Backup (*.ssdr_cfg)"));
    if (!self || !exportPath || self->m_exportPath != exportPath.data() || path.isEmpty()) {
        return;
    }
    exportPath->setText(path.endsWith(QStringLiteral(".ssdr_cfg"), Qt::CaseInsensitive)
                              ? path
                              : path + QStringLiteral(".ssdr_cfg"));
}

void ProfileImportExportDialog::chooseImportPath()
{
    const QPointer<ProfileImportExportDialog> self(this);
    const QPointer<QLineEdit> importPath(m_importPath);
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Profile Backup"),
        profileTransferDirectory(), QStringLiteral("Profile Backup (*.ssdr_cfg)"));
    if (!self || !importPath || self->m_importPath != importPath.data() || path.isEmpty()) {
        return;
    }
    importPath->setText(path);
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
    if (!m_model) { return; }
    const RadioControlSession session = m_model->controlSession();
    const QPointer<ProfileImportExportDialog> self(this);
    if (!m_importPath || m_importPath->text().trimmed().isEmpty()) {
        chooseImportPath();
        if (!self || !self->m_importPath || self->m_importPath->text().trimmed().isEmpty()) {
            return;
        }
    }
    rememberProfileTransferDirectory(m_importPath->text());
    if (!confirmImport() || !self || !m_model || !m_model->controlSessionCurrent(session)) { return; }
    m_transfer->importDatabase(m_importPath->text(), session);
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
