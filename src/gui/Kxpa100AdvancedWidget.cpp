#include "Kxpa100AdvancedWidget.h"
#include "CanonIndicators.h"
#include "models/Kxpa100Model.h"
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <array>
#include <algorithm>
#include <cmath>
namespace AetherSDR
{
namespace
{
void available(QWidget* widget, const QString& reason)
{
    widget->setEnabled(reason.isEmpty());
    widget->setToolTip(reason);
    widget->setAccessibleDescription(reason);
}
bool save(const QString& path, const QByteArray& data, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
void fillTable(QTableWidget* table, int row, const QStringList& values)
{
    for (int c = 0; c < values.size(); ++c) {
        table->setItem(row, c, new QTableWidgetItem(values[c]));
    }
}
} // namespace
Kxpa100AdvancedWidget::Kxpa100AdvancedWidget(Kxpa100Model* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setObjectName("kxpa100Advanced");
    setAccessibleName(tr("KXPA100 Advanced settings and maintenance"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 4, 0, 0);
    root->setSpacing(6);
    const auto label = [this](const QString& text, const char* name, QLayout* layout) {
        auto* l = new QLabel(text, this);
        l->setObjectName(name);
        l->setTextFormat(Qt::PlainText);
        l->setWordWrap(true);
        layout->addWidget(l);
        return l;
    };
    const auto button = [this](const QString& title, const char* name, QLayout* layout) {
        auto* b = new QPushButton(title, this);
        b->setObjectName(name);
        b->setAccessibleName(title);
        b->setAutoDefault(false);
        b->setFixedHeight(22);
        layout->addWidget(b);
        return b;
    };
    applyCanonSheet(this, [] {
        return QStringLiteral(
            "QWidget#kxpa100Advanced, QWidget#kxpa100Advanced QLabel, QWidget#kxpa100Advanced QCheckBox, "
            "QWidget#kxpa100Advanced QComboBox, QWidget#kxpa100Advanced QSpinBox, "
            "QWidget#kxpa100Advanced QTableWidget, QWidget#kxpa100Advanced QHeaderView::section, "
            "QWidget#kxpa100Advanced QLineEdit, QWidget#kxpa100Advanced QProgressBar { font-size: 10px; }"
            "QWidget#kxpa100Advanced QLabel { color: {{color.canon.muted}}; }"
            "QWidget#kxpa100Advanced QGroupBox { font-size: 10px; font-weight: bold; }"
            "QWidget#kxpa100Advanced QPushButton { font-size: 10px; padding: 0 4px; }"
            "QWidget#kxpa100Advanced QTableWidget::item { padding: 1px 3px; }"
            "QWidget#kxpa100Advanced QHeaderView::section { padding: 2px 3px; }");
    });
    m_identity = label({}, "kxAdvancedIdentity", root);
    m_availability = label({}, "kxAdvancedAvailability", root);
    auto* config = new QGroupBox(tr("Configuration — staged edits"), this);
    auto* configuration = new QVBoxLayout(config);
    auto* actions = new QHBoxLayout;
    auto* read = button(tr("Read configuration"), "kxReadConfiguration", actions);
    configuration->addLayout(actions);
    label(tr("Read before editing. ANT1 and ANT2 checkboxes stage enabled antennas per band. "
        "The All bands row applies a deliberate change to every band. At least one antenna must remain enabled. "
        "Apply verifies device readback; Cancel edits discards only staging."),"kxConfigurationHelp",configuration);
    m_configuration = new QTableWidget(12,4,this);
    m_configuration->setObjectName("kxConfiguration");
    m_configuration->setAccessibleName(tr("Confirmed and staged KXPA100 antenna enables"));
    m_configuration->setHorizontalHeaderLabels({tr("Band"),tr("ANT1"),tr("ANT2"),tr("Readback")});
    m_configuration->verticalHeader()->hide();
    m_configuration->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_configuration->verticalHeader()->setMinimumSectionSize(22);
    m_configuration->verticalHeader()->setDefaultSectionSize(22);
    m_configuration->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_configuration->setMinimumHeight(292); m_configuration->setMaximumHeight(310);
    m_configuration->setWordWrap(false);
    configuration->addWidget(m_configuration);
    auto* edits = new QVBoxLayout;
    auto* apply = button(tr("Apply changed fields"), "kxApplyConfiguration", edits);
    auto* discard = button(tr("Cancel edits"), "kxDiscardConfiguration", edits);
    configuration->addLayout(edits);
    root->addWidget(config);
    auto* backupGroup = new QGroupBox(tr("Complete native backup and restore"), this);
    auto* backup = new QVBoxLayout(backupGroup);
    auto* files = new QVBoxLayout;
    auto* saveBackup = button(tr("Read and save backup…"), "kxSaveBackup", files);
    auto* choose = button(tr("Choose backup…"), "kxChooseBackup", files);
    backup->addLayout(files);
    m_preview = label(tr("No backup selected."), "kxBackupPreview", backup);
    label(tr("A .KXPAConfig backup contains both EEPROM regions (17,408 bytes). Restore verifies each "
             "allowed block and excludes amplifier identity and fault history. Cancellation or failure cannot "
             "undo blocks already written. A successful verified restore requests a restart; reconnect to inspect settings."),
          "kxBackupHelp", backup);
    m_restoreConfirm =
        new QCheckBox(tr("Confirm backup replacement"), this);
    m_restoreConfirm->setObjectName("kxConfirmRestore");
    backup->addWidget(m_restoreConfirm);
    label(tr("Replace this tuner's settings and stored matches with the previewed backup."),
          "kxRestoreConfirmationHelp", backup);
    auto* restore = button(tr("Restore and verify"), "kxRestoreBackup", backup);
    root->addWidget(backupGroup);
    auto* faultGroup = new QGroupBox(tr("Stored fault history — newest first"), this);
    auto* faults = new QVBoxLayout(faultGroup);
    auto* faultActions = new QVBoxLayout;
    auto* readFaultsButton = button(tr("Read fault history"), "kxReadFaults", faultActions);
    auto* exportFaults = button(tr("Export CSV…"), "kxExportFaults", faultActions);
    faults->addLayout(faultActions);
    label(
        tr("Elapsed device time is not a calendar timestamp. Power On history is distinct from the current fault state. "
           "Unknown records retain raw bytes. Clear fault in the applet clears only the current fault."),
        "kxFaultHistoryHelp", faults);
    m_faults = new QTableWidget(0, 7, this);
    m_faults->setObjectName("kxStoredFaults");
    m_faults->setAccessibleName(tr("Stored tuner fault history"));
    m_faults->setHorizontalHeaderLabels({tr("Slot"), tr("Elapsed s"), tr("Fault"), tr("Freq kHz"), tr("Value"), tr("Unit"), tr("Raw bytes")});
    faults->addWidget(m_faults);
    root->addWidget(faultGroup);
    auto* memoriesGroup = new QGroupBox(tr("Memories and reset"), this);
    auto* memories = new QVBoxLayout(memoriesGroup);
    auto* memoryActions = new QVBoxLayout;
    m_memoryFrequency = new QSpinBox(this);
    m_memoryFrequency->setObjectName("kxInspectFrequency");
    m_memoryFrequency->setAccessibleName(
        tr("Memory inspection frequency in kHz"));
    m_memoryFrequency->setRange(0, 54000);
    m_memoryFrequency->setSuffix(tr(" kHz"));
    m_memoryFrequency->setSpecialValueText(tr("Select frequency"));
    memoryActions->addWidget(m_memoryFrequency);
    auto* inspect = button(tr("Inspect bin"), "kxInspectMemories", memoryActions);
    memories->addLayout(memoryActions);
    m_memorySummary = label(tr("No bin inspected."), "kxMemorySummary", memories);
    m_memories = new QTableWidget(0, 6, this);
    m_memories->setObjectName("kxStoredMemories");
    m_memories->setAccessibleName(tr("Stored matches, independent of live tuner state"));
    m_memories->setHorizontalHeaderLabels(
        {tr("ANT"), tr("Bypassed"), tr("C mask"), tr("L mask"), tr("C side"), tr("State")});
    memories->addWidget(m_memories);
    auto* eraseSelectors = new QHBoxLayout;
    m_eraseBand = new QComboBox(this);
    m_eraseBand->setObjectName("kxEraseBand");
    m_eraseBand->setAccessibleName(tr("Band to erase"));
    m_eraseBand->addItem(tr("All 11 bands"), 15);
    for (int i = 0; i < 11; ++i) {
        m_eraseBand->addItem(Kxpa100::bandName(i), i);
    }
    eraseSelectors->addWidget(m_eraseBand);
    m_eraseAntenna = new QComboBox(this);
    m_eraseAntenna->setObjectName("kxEraseAntenna");
    m_eraseAntenna->setAccessibleName(tr("Antenna memories to erase"));
    m_eraseAntenna->addItem(tr("All antennas"), 0);
    for (int i = 1; i <= 2; ++i) {
        m_eraseAntenna->addItem(tr("ANT %1").arg(i), i);
    }
    eraseSelectors->addWidget(m_eraseAntenna);
    memories->addLayout(eraseSelectors);
    auto* eraseTarget = label({}, "kxEraseTarget", memories);
    m_eraseConfirm = new QCheckBox(tr("Confirm selected erase"), this);
    m_eraseConfirm->setObjectName("kxConfirmErase");
    memories->addWidget(m_eraseConfirm);
    auto* erase = button(tr("Erase selected memories"), "kxEraseMemories", memories);
    m_resetConfirm = new QCheckBox(
        tr("Confirm factory reset"), this);
    m_resetConfirm->setObjectName("kxConfirmReset");
    memories->addWidget(m_resetConfirm);
    label(tr("Erase settings and stored matches, restart, and reconnect at the default baud."),
          "kxResetConfirmationHelp", memories);
    auto* reset = button(tr("Factory reset"), "kxFactoryReset", memories);
    root->addWidget(memoriesGroup);
    for (auto* table : {m_faults, m_memories}) {
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMinimumHeight(150);
        table->setMaximumHeight(230);
    }
    m_progress = new QProgressBar(this);
    m_progress->setObjectName("kxMaintenanceProgress");
    m_progress->setAccessibleName(tr("Tuner operation progress"));
    root->addWidget(m_progress);
    m_status = label(tr("Ready. No operation has run."), "kxMaintenanceStatus", root);
    auto* cancel = button(tr("Cancel operation"), "kxCancelMaintenance", root);
    connect(m_configuration,&QTableWidget::itemChanged,this,[this](QTableWidgetItem* item) {
        if (item->column()==1 || item->column()==2) { stageField(item->row(),item->column()); }
    });
    connect(m_memoryFrequency,&QSpinBox::valueChanged,this,&Kxpa100AdvancedWidget::refreshAvailability);
    connect(discard, &QPushButton::clicked, this, [this] {
        m_staged.reset();
        renderConfiguration();
        refreshAvailability();
    });
    connect(read, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_staged.reset();
            m_ownOperation = true;
            m_model->readConfiguration();
        }
    });
    connect(apply, &QPushButton::clicked, this, [this] {
        if (m_model && m_staged.has_value()) {
            m_applying = true;
            m_ownOperation = true;
            m_model->applyConfiguration(*m_staged);
        }
    });
    connect(saveBackup, &QPushButton::clicked, this, [this] { openFileDialog(FileAction::Backup); });
    connect(choose, &QPushButton::clicked, this, [this] { openFileDialog(FileAction::Choose); });
    connect(exportFaults, &QPushButton::clicked, this, [this] { openFileDialog(FileAction::Faults); });
    connect(restore, &QPushButton::clicked, this, [this] {
        if (m_model && m_backup && restoreReason().isEmpty() && m_restoreConfirm->isChecked()) {
            m_ownOperation = true;
            m_model->restoreBackup(*m_backup, true);
        }
    });
    connect(readFaultsButton, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_ownOperation = true;
            m_model->readFaultHistory();
        }
    });
    connect(inspect, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_ownOperation = true;
            m_model->inspectMemories(m_memoryFrequency->value());
        }
    });
    connect(erase, &QPushButton::clicked, this, [this] {
        if (m_model && m_eraseConfirm->isChecked()) {
            m_ownOperation = true;
            m_model->eraseMemories(m_eraseBand->currentData().toInt(), m_eraseAntenna->currentData().toInt(),
                                   true);
        }
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (m_model && m_resetConfirm->isChecked()) {
            m_ownOperation = true;
            m_model->factoryReset(true);
        }
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (m_model && m_ownOperation) {
            m_model->cancelOperation();
        }
    });
    for (auto* check : {m_restoreConfirm, m_eraseConfirm, m_resetConfirm}) {
        check->setAccessibleName(check == m_restoreConfirm
                                     ? tr("Confirm replacing this tuner's settings and stored matches with the previewed backup")
                                     : check == m_resetConfirm
                                           ? tr("Confirm factory reset: erase settings and matches, restart, and reconnect at the default baud")
                                           : check->text());
        connect(check, &QCheckBox::toggled, this, &Kxpa100AdvancedWidget::refreshAvailability);
    }
    const auto targetChanged = [this, eraseTarget] {
        m_eraseConfirm->setChecked(false);
        const QString target = tr("Erase target: %1 / %2").arg(m_eraseBand->currentText(), m_eraseAntenna->currentText());
        eraseTarget->setText(target);
        m_eraseConfirm->setAccessibleName(tr("Confirm selected erase. %1").arg(target));
        refreshAvailability();
    };
    connect(m_eraseBand, &QComboBox::currentIndexChanged, this, targetChanged);
    connect(m_eraseAntenna, &QComboBox::currentIndexChanged, this, targetChanged);
    targetChanged();
    if (model) {
        connect(model, &Kxpa100Model::configurationRead, this, &Kxpa100AdvancedWidget::configurationRead);
        connect(model, &Kxpa100Model::availabilityChanged, this, &Kxpa100AdvancedWidget::refreshAvailability);
        connect(model, &Kxpa100Model::statusUpdated, this, &Kxpa100AdvancedWidget::refreshAvailability);
        connect(model, &Kxpa100Model::operationBusyChanged, this, &Kxpa100AdvancedWidget::refreshAvailability);
        connect(model, &Kxpa100Model::connected, this, &Kxpa100AdvancedWidget::clearDeviceData);
        connect(model, &Kxpa100Model::disconnected, this, &Kxpa100AdvancedWidget::clearDeviceData);
        connect(model, &Kxpa100Model::operationProgress, this, [this](int done, int total) {
            if (m_ownOperation) {
                m_progress->setRange(0, total);
                m_progress->setValue(done);
            }
        });
        connect(model, &Kxpa100Model::operationFinished, this, [this](const QString& message) {
            if (m_ownOperation) {
                if (m_applying) {
                    m_staged.reset();
                    m_confirmed = m_model->configuration();
                    renderConfiguration();
                }
                m_applying = false;
                m_ownOperation = false;
                m_restoreConfirm->setChecked(false);
                m_eraseConfirm->setChecked(false);
                m_resetConfirm->setChecked(false);
                showStatus(message);
                refreshAvailability();
            }
        });
        connect(model, &Kxpa100Model::operationFailed, this, [this](const QString& reason) {
            if (m_ownOperation) {
                m_applying = false;
                m_ownOperation = false;
                m_savePath.clear();
                showStatus(
                    reason +
                    tr(" Confirmed changes may already have applied; review readbacks before retrying."));
                renderConfiguration();
                refreshAvailability();
            }
        });
        connect(model, &Kxpa100Model::backupRead, this, [this](const Kxpa100::Backup& data) {
            if (m_savePath.isEmpty()) {
                return;
            }
            QString error;
            const QString path = m_savePath;
            m_savePath.clear();
            const bool saved = Kxpa100Model::saveBackupFile(path, data, &error);
            showStatus(saved ? tr("Complete backup saved: %1").arg(path)
                             : tr("Backup save failed: %1").arg(error));
        });
        connect(model, &Kxpa100Model::faultHistoryRead, this, &Kxpa100AdvancedWidget::readFaults);
        connect(model, &Kxpa100Model::memoriesRead, this, &Kxpa100AdvancedWidget::readMemories);
        connect(model, &QObject::destroyed, this, [this] {
            m_model = nullptr;
            clearDeviceData();
        });
    }
    renderConfiguration();
    refreshAvailability();
}
Kxpa100AdvancedWidget::~Kxpa100AdvancedWidget()
{
    if (m_fileDialog) {
        QFileDialog* dialog = m_fileDialog.data();
        m_fileDialog.clear();
        disconnect(dialog, nullptr, this, nullptr);
        dialog->reject();
        // Native sheets can still be unwinding their completion callback.
        // QWidget child teardown must not delete them synchronously.
        dialog->setParent(nullptr);
        dialog->deleteLater();
    }
    if (m_model && m_ownOperation) {
        m_model->cancelOperation();
    }
}
void Kxpa100AdvancedWidget::showStatus(const QString& message)
{
    m_status->setText(message);
    m_status->setAccessibleDescription(message);
    QAccessibleEvent event(m_status, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
QString Kxpa100AdvancedWidget::schemaReason(bool writing) const
{
    if (!m_model) {
        return tr("Connect a KXPA100 first.");
    }
    const QString reason = m_model->unavailableReason(writing);
    if (!reason.isEmpty()) {
        return reason;
    }
    if (m_model->status().firmware != "01.39") {
        return tr("Configuration and memory schema support is verified for firmware 01.39 only. Use the "
                  "official Utility for other firmware.");
    }
    return {};
}
QString Kxpa100AdvancedWidget::restoreReason() const
{
    const QString reason = schemaReason(true);
    if (!reason.isEmpty()) {
        return reason;
    }
    if (!m_backup) {
        return tr("Choose a complete validated .KXPAConfig backup first.");
    }
    if (m_backup->firmware != m_model->status().firmware || m_backup->serial != m_model->status().serial) {
        return tr("Backup firmware and serial must match this amplifier.");
    }
    return {};
}
void Kxpa100AdvancedWidget::refreshAvailability()
{
    const QString readReason = schemaReason(false), writeReason = schemaReason(true);
    const QString tunerWrite = !writeReason.isEmpty() ? writeReason
        : m_model->unavailableReason(true, true);
    const QString tunerRead = !readReason.isEmpty() ? readReason
        : m_model->unavailableReason(false, true);
    const bool busy = m_model && m_model->operationBusy();
    m_identity->setText(m_model ? tr("%1 · %2 · S/N %3 · Firmware %4")
                                      .arg(m_model->description(), m_model->status().identity,
                                           m_model->status().serial, m_model->status().firmware)
                                : tr("Disconnected"));
    m_availability->setText(!readReason.isEmpty() ? readReason
                            : writeReason.isEmpty()
                                ? tr("Ready. Apply sends changed fields and verifies readback.")
                                : writeReason);
    for (const char* name :
         {"kxReadConfiguration", "kxSaveBackup", "kxReadFaults", "kxInspectMemories"}) {
        available(findChild<QPushButton*>(name), readReason);
    }
    available(m_configuration, !tunerRead.isEmpty() ? tunerRead
                               : !m_confirmed
                                   ? tr("Read complete configuration before editing.")
                                   : QString());
    available(findChild<QPushButton*>("kxApplyConfiguration"), !tunerWrite.isEmpty() ? tunerWrite
                                                                : !m_staged
                                                                    ? tr("Deliberately edit a field first.")
                                                                    : QString());
    available(findChild<QPushButton*>("kxDiscardConfiguration"),
              busy ? tr("Wait for the operation to finish.") : QString());
    available(findChild<QPushButton*>("kxChooseBackup"),
              busy ? tr("Wait for the operation to finish.") : QString());
    available(m_restoreConfirm, restoreReason());
    available(findChild<QPushButton*>("kxRestoreBackup"),
              !restoreReason().isEmpty()       ? restoreReason()
              : !m_restoreConfirm->isChecked() ? tr("Confirm replacement of stored settings first.")
                                               : QString());
    available(findChild<QPushButton*>("kxExportFaults"), !m_faultsLoaded ? tr("Read fault history first.")
                                                          : busy ? tr("Wait for the operation to finish.")
                                                                 : QString());
    available(findChild<QPushButton*>("kxReadConfiguration"),tunerRead);
    available(findChild<QPushButton*>("kxInspectMemories"),!tunerRead.isEmpty() ? tunerRead
        : Kxpa100::isSupportedFrequency(m_memoryFrequency->value()) ? QString() : tr("Select 1800–54000 kHz."));
    available(m_eraseConfirm, tunerWrite);
    available(m_resetConfirm, writeReason);
    available(findChild<QPushButton*>("kxEraseMemories"),
              !tunerWrite.isEmpty()          ? tunerWrite
              : !m_eraseConfirm->isChecked() ? tr("Confirm the selected band and antenna erase target first.")
                                             : QString());
    available(findChild<QPushButton*>("kxFactoryReset"),
              !writeReason.isEmpty()         ? writeReason
              : !m_resetConfirm->isChecked() ? tr("Confirm factory reset and default-baud reconnect first.")
                                             : QString());
    available(findChild<QPushButton*>("kxCancelMaintenance"),
              !busy || !m_ownOperation ? tr("No operation is running from this panel.") : QString());
    m_progress->setVisible(busy && m_ownOperation);
    available(m_eraseBand, busy ? tr("Wait for the operation to finish.") : QString());
    available(m_eraseAntenna, busy ? tr("Wait for the operation to finish.") : QString());
}
void Kxpa100AdvancedWidget::configurationRead(const Kxpa100::Configuration& values)
{
    m_confirmed=values;
    if (m_staged && *m_staged==values) { m_staged.reset(); }
    renderConfiguration(); refreshAvailability();
}
void Kxpa100AdvancedWidget::renderConfiguration()
{
    const QSignalBlocker blocker(m_configuration);
    for (int row=0;row<12;++row) {
        auto* name=new QTableWidgetItem(row==0 ? tr("All bands") : Kxpa100::bandName(row-1));
        name->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable); m_configuration->setItem(row,0,name);
        const auto values=m_staged ? m_staged : m_confirmed;
        for (int antenna=1;antenna<=2;++antenna) {
            auto* item=new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);
            int on=0; const int start=row==0 ? 0 : row-1, end=row==0 ? 10 : row-1;
            for (int band=start;band<=end;++band) { if (values && ((*values)[band] & (1<<(antenna-1)))) { ++on; } }
            item->setCheckState(on==0 ? Qt::Unchecked : on==end-start+1 ? Qt::Checked : Qt::PartiallyChecked);
            item->setData(Qt::AccessibleTextRole,tr("%1 ANT%2 enabled").arg(name->text()).arg(antenna));
            item->setToolTip(tr("Stage this antenna enable for %1; Apply sends it to the device.").arg(name->text()));
            item->setData(Qt::AccessibleDescriptionRole,item->toolTip()); m_configuration->setItem(row,antenna,item);
        }
        QString readback=tr("Unknown");
        if (m_confirmed) {
            const auto value=(*m_confirmed)[row==0 ? 0 : row-1];
            const bool mixed=row==0 && std::any_of(m_confirmed->begin(),m_confirmed->end(),[value](int mask) { return mask!=value; });
            readback=mixed ? tr("Mixed") : value==3 ? tr("ANT1 + 2") : tr("ANT%1").arg(value);
        }
        auto* actual=new QTableWidgetItem(readback); actual->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable);
        m_configuration->setItem(row,3,actual);
    }
}
void Kxpa100AdvancedWidget::stageField(int row,int column)
{
    if (!m_confirmed || row<0 || row>=12 || column<1 || column>2) { return; }
    const bool on=m_configuration->item(row,column)->checkState()==Qt::Checked;
    Kxpa100::Configuration staged=m_staged.value_or(*m_confirmed);
    const int start=row==0 ? 0 : row-1, end=row==0 ? 10 : row-1, bit=1<<(column-1);
    for (int band=start;band<=end;++band) {
        staged[band]=on ? staged[band]|bit : staged[band]&~bit;
        if (staged[band]==0) { showStatus(tr("At least one antenna must remain enabled on every band.")); renderConfiguration(); return; }
    }
    m_staged=staged==*m_confirmed ? std::nullopt : std::optional<Kxpa100::Configuration>(staged);
    renderConfiguration(); refreshAvailability(); showStatus(tr("Antenna changes staged. Nothing has been sent."));
}
void Kxpa100AdvancedWidget::clearDeviceData()
{
    m_confirmed.reset();
    m_staged.reset();
    m_savePath.clear();
    m_faultRecords.clear();
    m_faultsLoaded = false;
    m_faults->setRowCount(0);
    m_memories->setRowCount(0);
    m_memorySummary->setText(tr("No bin inspected for this session."));
    m_restoreConfirm->setChecked(false);
    m_eraseConfirm->setChecked(false);
    m_resetConfirm->setChecked(false);
    m_applying = false;
    m_ownOperation = false;
    renderConfiguration();
    refreshAvailability();
}
void Kxpa100AdvancedWidget::openFileDialog(FileAction action)
{
    if (m_fileDialog) {
        m_fileDialog->raise();
        m_fileDialog->activateWindow();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName("kxMaintenanceFileDialog");
    dialog->setAccessibleName(tr("KXPA100 maintenance file selection"));
    dialog->setWindowTitle(action == FileAction::Choose   ? tr("Choose KXPA100 backup")
                           : action == FileAction::Backup ? tr("Save complete KXPA100 backup")
                                                          : tr("Export fault history"));
    dialog->setNameFilter(action == FileAction::Faults
                              ? tr("CSV files (*.csv)")
                              : tr("KXPA100 configuration (*.KXPAConfig);;All files (*)"));
    dialog->setAcceptMode(action == FileAction::Choose ? QFileDialog::AcceptOpen : QFileDialog::AcceptSave);
    dialog->setFileMode(action == FileAction::Choose ? QFileDialog::ExistingFile : QFileDialog::AnyFile);
    dialog->setDefaultSuffix(action == FileAction::Faults ? "csv" : "KXPAConfig");
    if (action != FileAction::Choose) {
        dialog->selectFile(
            action == FileAction::Faults
                ? "kxpa100-fault-history.csv"
                : QStringLiteral("SN%1.KXPAConfig").arg(m_model ? m_model->status().serial : QString()));
    }
    connect(dialog, &QDialog::finished, this, [this, dialog, action](int result) {
        const QStringList paths = dialog->selectedFiles();
        m_fileDialog.clear();
        dialog->deleteLater();
        if (result != QDialog::Accepted || paths.isEmpty()) {
            return;
        }
        if (action == FileAction::Choose) {
            loadBackup(paths.first());
        } else if (action == FileAction::Backup) {
            const QString reason = schemaReason(false);
            if (!reason.isEmpty()) {
                showStatus(reason);
                return;
            }
            m_savePath = paths.first();
            m_ownOperation = true;
            if (!m_model->readBackup()) {
                m_savePath.clear();
            }
        } else {
            if (!m_faultsLoaded) {
                showStatus(tr("Read fault history again before exporting."));
                return;
            }
            QString error;
            const bool saved = save(paths.first(), Kxpa100Model::faultsCsv(m_faultRecords), &error);
            showStatus(saved ? tr("Fault history exported.") : error);
        }
    });
    dialog->open();
}
bool Kxpa100AdvancedWidget::loadBackup(const QString& path)
{
    m_backup.reset();
    m_restoreConfirm->setChecked(false);
    QFile file(path);
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
    } else if (file.size() > 64 * 1024) {
        error = tr("Backup exceeds the 64 KiB limit.");
    } else {
        m_backup = Kxpa100Model::decodeBackupFile(file.read(64 * 1024 + 1), &error);
    }
    m_preview->setText(m_backup ? tr("%1\nS/N %2 · Firmware %3 · 17,408 bytes · complete coverage and CRC "
                                     "verified\nRestore excludes identity and fault history.")
                                      .arg(QFileInfo(path).fileName(), m_backup->serial, m_backup->firmware)
                                : tr("No valid backup selected."));
    showStatus(m_backup ? tr("Backup validated for preview. No tuner settings changed.") : error);
    refreshAvailability();
    return m_backup.has_value();
}
void Kxpa100AdvancedWidget::readFaults(const QList<Kxpa100::FaultRecord>& records)
{
    m_faultRecords=records; m_faultsLoaded=true; m_faults->setRowCount(records.size());
    for (int i=0;i<records.size();++i) {
        const auto& r=records[i];
        fillTable(m_faults,i,{QString::number(r.slot),QString::number(r.elapsedSeconds),r.description,
            QString::number(r.frequencyKhz),QString::number(r.value,'f',3),r.unit,QString::fromLatin1(r.raw.toHex(' '))});
    }
    refreshAvailability();
}
void Kxpa100AdvancedWidget::readMemories(const Kxpa100::MemoryBin& bin)
{
    m_memorySummary->setText(tr("%1–%2 kHz · %3 · bin %4. Read-only inspection; live relays are unchanged.")
        .arg(bin.firstKhz).arg(bin.lastKhz).arg(Kxpa100::bandName(bin.band)).arg(bin.index));
    m_memories->setRowCount(bin.matches.size());
    for (int i=0;i<bin.matches.size();++i) {
        const auto& m=bin.matches[i];
        fillTable(m_memories,i,{QString::number(m.antenna),m.empty ? QStringLiteral("—") : m.bypassed ? tr("Yes") : tr("No"),
            m.empty ? QStringLiteral("—") : QString::number(m.capacitorMask,16).rightJustified(2,'0').toUpper(),
            m.empty ? QStringLiteral("—") : QString::number(m.inductorMask,16).rightJustified(2,'0').toUpper(),
            m.empty ? QStringLiteral("—") : m.side==Kxpa100::CapacitorSide::Antenna ? tr("Antenna") : tr("Transmitter"),
            m.empty ? tr("Empty") : tr("Stored")});
    }
}
} // namespace AetherSDR
