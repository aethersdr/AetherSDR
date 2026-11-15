#include "Kpa500AdvancedWidget.h"
#include "RadioSetupDialogCommon.h"
#include "core/Kpa500Connection.h"
#include "core/Kpa500FaultHistory.h"

#include <QAccessible>
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
void setAvailability(QWidget* widget, const QString& reason, const QString& help)
{
    widget->setEnabled(reason.isEmpty());
    const QString description = reason.isEmpty() ? help : reason;
    widget->setToolTip(description);
    widget->setAccessibleDescription(description);
}

bool saveAtomically(const QString& path, const QByteArray& bytes, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

QString csvField(QString value)
{
    value.replace('"', QStringLiteral("\"\""));
    return '"' + value + '"';
}
} // namespace

Kpa500AdvancedWidget::Kpa500AdvancedWidget(Kpa500Connection* connection, QWidget* parent)
    : QWidget(parent), m_connection(connection)
{
    setObjectName(QStringLiteral("kpa500Advanced"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(8);
    const auto label = [this](const QString& text, const char* name, Tone tone = Tone::Hint) {
        auto* result = new QLabel(text, this);
        result->setObjectName(QString::fromLatin1(name));
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setTone(result, tone);
        return result;
    };
    const auto button = [this](const QString& text, const char* name) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(QString::fromLatin1(name));
        result->setAccessibleName(text);
        return result;
    };
    m_identity = label({}, "kpaAdvancedIdentity", Tone::Value);
    layout->addWidget(m_identity);
    layout->addWidget(label(tr("Maintenance for KPA500 firmware 01.54. Reads run only when requested. "
                              "Writes require Standby and zero RF; keep the amplifier idle until completion."),
                            "kpaAdvancedHelp"));
    m_availability = label({}, "kpaMaintenanceAvailability");
    layout->addWidget(m_availability);

    auto* backupGroup = new QGroupBox(tr("Configuration backup and restore"), this);
    auto* backupLayout = new QVBoxLayout(backupGroup);
    auto* backupButtons = new QHBoxLayout;
    m_saveBackup = button(tr("Save backup…"), "kpaSaveBackup");
    m_chooseBackup = button(tr("Choose backup…"), "kpaChooseBackup");
    backupButtons->addWidget(m_saveBackup);
    backupButtons->addWidget(m_chooseBackup);
    backupButtons->addStretch();
    backupLayout->addLayout(backupButtons);
    m_backupPreview = label(tr("Choose a .KPAConfig file to review before restoring."), "kpaBackupPreview");
    backupLayout->addWidget(m_backupPreview);
    m_confirmRestore = new QCheckBox(tr("Replace this amplifier’s stored configuration"), this);
    m_confirmRestore->setObjectName(QStringLiteral("kpaConfirmRestore"));
    m_confirmRestore->setAccessibleName(m_confirmRestore->text());
    backupLayout->addWidget(m_confirmRestore);
    m_restore = button(tr("Restore selected backup"), "kpaRestoreBackup");
    setButtonRole(m_restore, ButtonRole::Danger);
    backupLayout->addWidget(m_restore, 0, Qt::AlignLeft);
    backupLayout->addWidget(label(tr("Restores 224 configuration bytes, including the banner. The checksum, "
                                    "serial number and firmware must match. Fault history is not restored. "
                                    "After verification, power-cycle the amplifier manually when ready."),
                                  "kpaRestoreHelp"));
    layout->addWidget(backupGroup);

    auto* bannerGroup = new QGroupBox(tr("Power-on banner"), this);
    auto* bannerLayout = new QVBoxLayout(bannerGroup);
    m_bannerReadback = label(tr("Banner has not been read."), "kpaBannerReadback", Tone::Value);
    bannerLayout->addWidget(m_bannerReadback);
    m_banner = new QLineEdit(this);
    m_banner->setObjectName(QStringLiteral("kpaBannerText"));
    m_banner->setMaxLength(8);
    m_banner->setAccessibleName(tr("Power-on banner, up to eight ASCII characters"));
    m_banner->setPlaceholderText(tr("Up to 8 characters"));
    bannerLayout->addWidget(m_banner);
    auto* bannerButtons = new QHBoxLayout;
    m_readBanner = button(tr("Read banner"), "kpaReadBanner");
    m_writeBanner = button(tr("Save banner"), "kpaWriteBanner");
    bannerButtons->addWidget(m_readBanner);
    bannerButtons->addWidget(m_writeBanner);
    bannerButtons->addStretch();
    bannerLayout->addLayout(bannerButtons);
    layout->addWidget(bannerGroup);

    auto* faultGroup = new QGroupBox(tr("Stored fault history"), this);
    auto* faultLayout = new QVBoxLayout(faultGroup);
    auto* faultButtons = new QHBoxLayout;
    m_readFaults = button(tr("Refresh fault history"), "kpaReadFaults");
    m_exportFaults = button(tr("Export CSV…"), "kpaExportFaults");
    faultButtons->addWidget(m_readFaults);
    faultButtons->addWidget(m_exportFaults);
    faultButtons->addStretch();
    faultLayout->addLayout(faultButtons);
    m_faultSummary = label(tr("No fault history captured. Uptime is elapsed amplifier time, not a calendar timestamp."),
                           "kpaFaultSummary");
    faultLayout->addWidget(m_faultSummary);
    m_faults = new QTableWidget(0, 6, this);
    m_faults->setObjectName(QStringLiteral("kpaStoredFaults"));
    m_faults->setAccessibleName(tr("KPA500 stored fault history, newest first"));
    m_faults->setHorizontalHeaderLabels({tr("Slot"), tr("Uptime (s)"), tr("Band"), tr("Fault"), tr("Raw data"), tr("Details")});
    m_faults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_faults->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_faults->setAlternatingRowColors(true);
    m_faults->setWordWrap(false);
    m_faults->verticalHeader()->hide();
    m_faults->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_faults->horizontalHeader()->setStretchLastSection(true);
    m_faults->setMinimumHeight(150);
    m_faults->setMaximumHeight(260);
    faultLayout->addWidget(m_faults);
    layout->addWidget(faultGroup);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("kpaMaintenanceProgress"));
    m_progress->setAccessibleName(tr("Amplifier maintenance progress"));
    m_progress->hide();
    layout->addWidget(m_progress);
    m_status = label(tr("Ready. No maintenance operation has run."), "kpaMaintenanceStatus");
    layout->addWidget(m_status);
    m_cancel = button(tr("Cancel operation"), "kpaCancelMaintenance");
    layout->addWidget(m_cancel, 0, Qt::AlignLeft);

    connect(m_saveBackup, &QPushButton::clicked, this, &Kpa500AdvancedWidget::saveBackup);
    connect(m_chooseBackup, &QPushButton::clicked, this, &Kpa500AdvancedWidget::chooseBackup);
    connect(m_confirmRestore, &QCheckBox::toggled, this, &Kpa500AdvancedWidget::refreshAvailability);
    connect(m_banner, &QLineEdit::textChanged, this, &Kpa500AdvancedWidget::refreshAvailability);
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!m_connection || !restoreUnavailableReason().isEmpty() || !m_confirmRestore->isChecked()) {
            return;
        }
        beginOperation(Operation::Restore);
        m_connection->restoreConfiguration(m_backup->data, m_backup->serial, m_backup->firmware);
    });
    connect(m_readBanner, &QPushButton::clicked, this, [this] {
        if (m_connection) {
            beginOperation(Operation::BannerRead);
            m_connection->readPowerOnBanner();
        }
    });
    connect(m_writeBanner, &QPushButton::clicked, this, [this] {
        if (m_connection && m_bannerLoaded) {
            beginOperation(Operation::BannerWrite);
            m_connection->writePowerOnBanner(m_banner->text());
        }
    });
    connect(m_readFaults, &QPushButton::clicked, this, [this] {
        if (m_connection) {
            beginOperation(Operation::FaultRead);
            m_connection->readFaultHistory();
        }
    });
    connect(m_exportFaults, &QPushButton::clicked, this, &Kpa500AdvancedWidget::exportFaults);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_connection && m_operation != Operation::None) {
            m_connection->cancelMaintenance();
        }
    });
    if (connection) {
        connect(connection, &Kpa500Connection::connected, this, &Kpa500AdvancedWidget::clearDeviceData);
        connect(connection, &Kpa500Connection::disconnected, this, &Kpa500AdvancedWidget::clearDeviceData);
        connect(connection, &Kpa500Connection::statusUpdated, this, &Kpa500AdvancedWidget::refreshAvailability);
        connect(connection, &Kpa500Connection::respondingChanged, this, &Kpa500AdvancedWidget::refreshAvailability);
        connect(connection, &Kpa500Connection::maintenanceAvailabilityChanged, this, &Kpa500AdvancedWidget::refreshAvailability);
        connect(connection, &Kpa500Connection::maintenanceBusyChanged, this, &Kpa500AdvancedWidget::refreshAvailability);
        connect(connection, &Kpa500Connection::maintenanceProgress, this, [this](int done, int total) {
            if (m_operation != Operation::None) {
                m_progress->setRange(0, total);
                m_progress->setValue(done);
            }
        });
        connect(connection, &Kpa500Connection::configurationRead, this, &Kpa500AdvancedWidget::configurationRead);
        connect(connection, &Kpa500Connection::faultHistoryRead, this, &Kpa500AdvancedWidget::faultHistoryRead);
        connect(connection, &Kpa500Connection::powerOnBannerRead, this, [this](const QString& text) {
            if (m_operation == Operation::BannerRead || m_operation == Operation::BannerWrite) {
                m_confirmedBanner = text;
                m_bannerLoaded = true;
                m_banner->setText(text);
                m_bannerReadback->setText(tr("Confirmed banner: “%1”").arg(text));
            }
        });
        connect(connection, &Kpa500Connection::maintenanceFinished, this, &Kpa500AdvancedWidget::operationFinished);
        connect(connection, &Kpa500Connection::maintenanceFailed, this, [this](const QString& message) {
            if (m_operation != Operation::None) {
                m_resultError = true;
                m_resultMessage = message;
                operationFinished(message);
            }
        });
        connect(connection, &QObject::destroyed, this, [this] {
            m_connection = nullptr;
            clearDeviceData();
        });
    }
    refreshAvailability();
}

Kpa500AdvancedWidget::~Kpa500AdvancedWidget()
{
    if (m_connection && m_operation != Operation::None) {
        m_connection->cancelMaintenance();
    }
}

void Kpa500AdvancedWidget::showStatus(const QString& text, bool error)
{
    m_status->setText(text);
    m_status->setAccessibleDescription(text);
    setTone(m_status, error ? Tone::Error : Tone::Value);
    QAccessibleEvent event(m_status, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

void Kpa500AdvancedWidget::beginOperation(Operation operation)
{
    m_operation = operation;
    m_operationSerial = m_connection->lastStatus().serialNumber.value_or(QString());
    m_operationFirmware = m_connection->lastStatus().firmwareVersion.value_or(QString());
    m_resultMessage.clear();
    m_resultError = false;
    m_progress->setRange(0, 0);
    m_progress->show();
    showStatus(tr("Maintenance in progress. Keep the amplifier idle."));
}

void Kpa500AdvancedWidget::operationFinished(const QString& message)
{
    if (m_operation == Operation::None) {
        return;
    }
    if (m_operation == Operation::Restore || (m_operation == Operation::BannerWrite && m_resultError)) {
        m_bannerLoaded = false;
        m_bannerReadback->setText(tr("Read the banner again to confirm current amplifier memory."));
    }
    m_confirmRestore->setChecked(false);
    m_operation = Operation::None;
    m_progress->hide();
    showStatus(m_resultMessage.isEmpty() ? message : m_resultMessage, m_resultError);
    refreshAvailability();
}

void Kpa500AdvancedWidget::clearDeviceData()
{
    m_bannerLoaded = false;
    m_confirmedBanner.clear();
    m_banner->clear();
    m_bannerReadback->setText(tr("Banner has not been read."));
    m_faultsLoaded = false;
    m_faults->setRowCount(0);
    m_faultSummary->setText(tr("No fault history captured for this connection."));
    m_confirmRestore->setChecked(false);
    refreshAvailability();
}

QString Kpa500AdvancedWidget::restoreUnavailableReason() const
{
    if (!m_connection) {
        return tr("Connect an amplifier first.");
    }
    const QString reason = m_connection->maintenanceUnavailableReason(true);
    if (!reason.isEmpty()) {
        return reason;
    }
    if (!m_backup) {
        return tr("Choose a valid .KPAConfig file first.");
    }
    if (m_backup->serial != m_connection->lastStatus().serialNumber
        || m_backup->firmware != m_connection->lastStatus().firmwareVersion) {
        return tr("The backup serial number and firmware must match the connected amplifier.");
    }
    return {};
}

void Kpa500AdvancedWidget::refreshAvailability()
{
    const QString readReason = m_connection ? m_connection->maintenanceUnavailableReason(false)
                                           : tr("Connect an amplifier first.");
    const QString writeReason = m_connection ? m_connection->maintenanceUnavailableReason(true) : readReason;
    const bool busy = m_connection && m_connection->maintenanceBusy();
    if (m_connection) {
        const Kpa500::Status& state = m_connection->lastStatus();
        m_identity->setText(tr("%1 · S/N %2 · Firmware %3")
            .arg(m_connection->description(), state.serialNumber.value_or(tr("unknown")),
                 state.firmwareVersion.value_or(tr("unknown"))));
    } else {
        m_identity->setText(tr("No amplifier connection."));
    }
    m_availability->setText(!readReason.isEmpty() ? readReason
        : (!writeReason.isEmpty() ? writeReason : tr("Ready for maintenance. Writes are verified by readback.")));
    setAvailability(m_saveBackup, readReason, tr("Read and save 224 configuration bytes to a .KPAConfig file."));
    setAvailability(m_readBanner, readReason, tr("Read the amplifier’s stored power-on banner."));
    setAvailability(m_readFaults, readReason, tr("Read the amplifier’s stored fault log."));
    setAvailability(m_chooseBackup, busy ? tr("Wait for maintenance to finish.") : QString(),
                    tr("Open and validate a backup locally. This does not write to the amplifier."));
    const QString restoreReason = restoreUnavailableReason();
    setAvailability(m_confirmRestore, restoreReason, tr("Confirm the selected backup will replace stored settings."));
    setAvailability(m_restore, !restoreReason.isEmpty() ? restoreReason
        : (!m_confirmRestore->isChecked() ? tr("Confirm replacement of the stored configuration first.") : QString()),
        tr("Write the selected configuration and verify every byte. Does not reboot the amplifier."));
    QString bannerReason = writeReason;
    if (bannerReason.isEmpty() && !m_bannerLoaded) {
        bannerReason = tr("Read the current banner first.");
    }
    setAvailability(m_banner, bannerReason, tr("Up to eight printable ASCII characters; spaces are preserved."));
    if (bannerReason.isEmpty() && m_banner->text() == m_confirmedBanner) {
        bannerReason = tr("Edit the banner before saving.");
    }
    for (const QChar ch : m_banner->text()) {
        if (ch.unicode() < 0x20 || ch.unicode() > 0x7e) {
            bannerReason = tr("Use printable ASCII characters only.");
            break;
        }
    }
    setAvailability(m_writeBanner, bannerReason, tr("Write the banner and confirm its stored bytes by readback."));
    setAvailability(m_exportFaults, !m_faultsLoaded ? tr("Read fault history first.")
        : (busy ? tr("Wait for maintenance to finish.") : QString()), tr("Export the captured fault table as CSV."));
    setAvailability(m_cancel, !busy || m_operation == Operation::None ? tr("No operation is running in this panel.") : QString(),
                    tr("Stop after the current request. Bytes already written cannot be undone."));
}

void Kpa500AdvancedWidget::openFileDialog(FileAction action)
{
    // Native file dialogs must not nest: canceling overlapping Windows COM
    // dialogs can invalidate a dialog that is still unwinding its modal loop.
    if (m_fileDialog) {
        m_fileDialog->raise();
        m_fileDialog->activateWindow();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName(QStringLiteral("kpaMaintenanceFileDialog"));
    if (action == FileAction::ExportFaults) {
        dialog->setWindowTitle(tr("Export KPA500 fault history"));
        dialog->setNameFilter(tr("CSV files (*.csv)"));
        dialog->setDefaultSuffix(QStringLiteral("csv"));
        dialog->selectFile(QStringLiteral("KPA500-fault-history.csv"));
    } else {
        dialog->setWindowTitle(action == FileAction::SaveBackup
            ? tr("Save KPA500 configuration") : tr("Choose KPA500 configuration"));
        dialog->setNameFilter(tr("KPA500 configuration (*.KPAConfig);;All files (*)"));
        dialog->setDefaultSuffix(QStringLiteral("KPAConfig"));
        if (action == FileAction::SaveBackup) {
            dialog->setDirectory(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
            dialog->selectFile(tr("SN%1.%2.KPAConfig").arg(
                m_connection->lastStatus().serialNumber.value_or(QString()),
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddTHHmmss"))));
        }
    }
    dialog->setAccessibleName(dialog->windowTitle());
    dialog->setAcceptMode(action == FileAction::ChooseBackup ? QFileDialog::AcceptOpen : QFileDialog::AcceptSave);
    dialog->setFileMode(action == FileAction::ChooseBackup ? QFileDialog::ExistingFile : QFileDialog::AnyFile);
    connect(dialog, &QDialog::finished, this, [this, dialog, action](int result) {
        const QStringList files = dialog->selectedFiles();
        m_fileDialog.clear();
        dialog->deleteLater();
        if (result != QDialog::Accepted || files.isEmpty() || files.first().isEmpty()) {
            return;
        }
        switch (action) {
        case FileAction::SaveBackup:
            saveBackupToFile(files.first());
            break;
        case FileAction::ChooseBackup:
            loadBackup(files.first());
            break;
        case FileAction::ExportFaults:
            exportFaultsToFile(files.first());
            break;
        }
    });
    dialog->open();
}

void Kpa500AdvancedWidget::saveBackup()
{
    if (!m_connection || !m_connection->maintenanceUnavailableReason(false).isEmpty()) {
        return;
    }
    openFileDialog(FileAction::SaveBackup);
}

void Kpa500AdvancedWidget::saveBackupToFile(const QString& path)
{
    const QString reason = m_connection ? m_connection->maintenanceUnavailableReason(false)
                                       : tr("Connect an amplifier first.");
    if (!reason.isEmpty()) {
        showStatus(tr("Backup was not started: %1").arg(reason), true);
        return;
    }
    m_savePath = path;
    beginOperation(Operation::Backup);
    m_connection->readConfiguration();
}

void Kpa500AdvancedWidget::configurationRead(const QByteArray& data)
{
    if (m_operation != Operation::Backup || !m_connection) {
        return;
    }
    if (m_operationSerial != m_connection->lastStatus().serialNumber
        || m_operationFirmware != m_connection->lastStatus().firmwareVersion) {
        m_resultError = true;
        m_resultMessage = tr("Amplifier identity changed. No backup was saved.");
        return;
    }
    const QByteArray encoded = Kpa500::encodeConfigurationBackup(
        {data, m_operationFirmware, m_operationSerial}, QDateTime::currentDateTime());
    QString error;
    if (encoded.isEmpty() || !saveAtomically(m_savePath, encoded, &error)) {
        m_resultError = true;
        m_resultMessage = tr("Backup was not saved: %1").arg(error.isEmpty() ? tr("invalid configuration data") : error);
        return;
    }
    m_resultMessage = tr("Backup saved: %1 · 224 bytes · S/N %2 · Firmware %3")
        .arg(m_savePath, m_operationSerial, m_operationFirmware);
}

void Kpa500AdvancedWidget::chooseBackup()
{
    openFileDialog(FileAction::ChooseBackup);
}

bool Kpa500AdvancedWidget::loadBackup(const QString& path)
{
    m_backup.reset();
    m_confirmRestore->setChecked(false);
    QFile file(path);
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
    } else if (file.size() > 65536) {
        error = tr("The file exceeds the 64 KiB limit.");
    } else {
        m_backup = Kpa500::decodeConfigurationBackup(file.read(65537), &error);
    }
    m_backupPath = path;
    if (m_backup) {
        m_backupPreview->setText(tr("%1\nS/N %2 · Firmware %3 · 224 bytes · CRC32 verified")
            .arg(QFileInfo(path).fileName(), m_backup->serial, m_backup->firmware));
        showStatus(tr("Backup loaded for review. No amplifier settings have changed."));
    } else {
        m_backupPreview->setText(tr("No valid backup selected."));
        showStatus(tr("Cannot load backup: %1").arg(error), true);
    }
    refreshAvailability();
    return m_backup.has_value();
}

void Kpa500AdvancedWidget::faultHistoryRead(const QByteArray& data)
{
    if (m_operation != Operation::FaultRead) {
        return;
    }
    QString error;
    const QVector<Kpa500::FaultEntry> entries = Kpa500::decodeFaultHistory(data, &error);
    if (!error.isEmpty()) {
        m_resultError = true;
        m_resultMessage = error;
        return;
    }
    m_faults->setRowCount(entries.size());
    for (qsizetype row = 0; row < entries.size(); ++row) {
        const Kpa500::FaultEntry& entry = entries.at(row);
        const QStringList fields{QString::number(entry.slot), QString::number(entry.uptimeSeconds), entry.band,
            tr("%1 · %2").arg(entry.code, 2, 16, QLatin1Char('0')).arg(entry.name),
            QStringLiteral("0x%1").arg(entry.data, 4, 16, QLatin1Char('0')).toUpper(), entry.description};
        for (int column = 0; column < fields.size(); ++column) {
            m_faults->setItem(row, column, new QTableWidgetItem(fields.at(column)));
        }
    }
    m_faultsLoaded = true;
    m_faultSummary->setText(tr("%1 records · S/N %2 · Captured %3\nNewest first. Uptime is amplifier time, not a date.")
        .arg(entries.size()).arg(m_operationSerial, QDateTime::currentDateTime().toString(Qt::ISODate)));
}

void Kpa500AdvancedWidget::exportFaults()
{
    if (!m_faultsLoaded) {
        return;
    }
    openFileDialog(FileAction::ExportFaults);
}

void Kpa500AdvancedWidget::exportFaultsToFile(const QString& path)
{
    if (!m_faultsLoaded) {
        showStatus(tr("Read fault history again before exporting."), true);
        return;
    }
    QString csv;
    for (int row = -1; row < m_faults->rowCount(); ++row) {
        QStringList fields;
        for (int column = 0; column < m_faults->columnCount(); ++column) {
            const QTableWidgetItem* item = row < 0 ? m_faults->horizontalHeaderItem(column) : m_faults->item(row, column);
            fields.append(csvField(item ? item->text() : QString()));
        }
        csv += fields.join(',') + QStringLiteral("\r\n");
    }
    QString error;
    const bool saved = saveAtomically(path, csv.toUtf8(), &error);
    showStatus(saved ? tr("Fault history exported: %1").arg(path) : tr("Export failed: %1").arg(error), !saved);
}
} // namespace AetherSDR
