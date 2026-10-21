#include "Kat500AdvancedWidget.h"
#include "CanonIndicators.h"
#include "models/Kat500Model.h"
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
#include <cmath>
namespace AetherSDR
{
namespace
{
struct Field {
    Kat500::Setting setting;
    int antenna;
    const char* title;
    const char* detail;
    double minimum, maximum;
    bool perBand;
};
const std::array<Field, 13> fields{
    {{Kat500::Setting::AmpKeyPower, 0, "Amp key limit (W)", "Amplifier key interrupt power (W; 1500 = unlimited)", 0, 1500,
      false},
     {Kat500::Setting::PowerOnDc, 0, "Power on with DC", "Power on when DC applied (0/1)", 0, 1, false},
     {Kat500::Setting::SleepIdle, 0, "Sleep when idle", "Sleep when idle (0/1)", 0, 1, false},
     {Kat500::Setting::EnabledAntenna, 1, "ANT 1 enabled", "ANT 1 enabled (0/1)", 0, 1, true},
     {Kat500::Setting::EnabledAntenna, 2, "ANT 2 enabled", "ANT 2 enabled (0/1)", 0, 1, true},
     {Kat500::Setting::EnabledAntenna, 3, "ANT 3 enabled", "ANT 3 enabled (0/1)", 0, 1, true},
     {Kat500::Setting::SlotsPerBin, 0, "ATU slots per bin", "ATU slots per bin (1–6, shared by antennas)", 1, 6, true},
     {Kat500::Setting::PreferredAntenna, 0, "Preferred antenna", "Preferred antenna (0 = last used, 1–3)", 0, 3, true},
     {Kat500::Setting::AutoSwr, 0, "Auto tune SWR", "Auto tune SWR threshold", 1.5, 99.99, true},
     {Kat500::Setting::BypassSwr, 0, "Bypass SWR", "Bypass SWR threshold", 1, 99.99, true},
     {Kat500::Setting::KeySwr, 0, "Amp interrupt SWR", "Amp key interrupt SWR threshold", 1, 99.99, true},
     {Kat500::Setting::AutoFineTune, 0, "Auto fine tune", "Auto fine tune (0/1)", 0, 1, true},
     {Kat500::Setting::RetuneDistance, 0, "Retune distance", "Additional: retune distance", 0, 65535, false}}};
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
Kat500AdvancedWidget::Kat500AdvancedWidget(Kat500Model* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setObjectName("kat500Advanced");
    setAccessibleName(tr("KAT500 Advanced settings and maintenance"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 4, 0, 0);
    root->setSpacing(6);
    const auto label = [this](const QString& text, const char* name, QLayout* layout) {
        auto* l = new QLabel(text, this);
        l->setObjectName(name);
        l->setAccessibleName(text);
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
            "QWidget#kat500Advanced, QWidget#kat500Advanced QLabel, QWidget#kat500Advanced QCheckBox, "
            "QWidget#kat500Advanced QComboBox, QWidget#kat500Advanced QSpinBox, "
            "QWidget#kat500Advanced QTableWidget, QWidget#kat500Advanced QHeaderView::section, "
            "QWidget#kat500Advanced QLineEdit, QWidget#kat500Advanced QProgressBar { font-size: 10px; }"
            "QWidget#kat500Advanced QLabel { color: {{color.canon.muted}}; }"
            "QWidget#kat500Advanced QGroupBox { font-size: 10px; font-weight: bold; }"
            "QWidget#kat500Advanced QPushButton { font-size: 10px; padding: 0 4px; }"
            "QWidget#kat500Advanced QTableWidget::item { padding: 1px 3px; }"
            "QWidget#kat500Advanced QHeaderView::section { padding: 2px 3px; }");
    });
    m_identity = label({}, "katAdvancedIdentity", root);
    m_availability = label({}, "katAdvancedAvailability", root);
    auto* config = new QGroupBox(tr("Configuration — staged edits"), this);
    auto* configuration = new QVBoxLayout(config);
    auto* actions = new QHBoxLayout;
    auto* read = button(tr("Read configuration"), "katReadConfiguration", actions);
    m_band = new QComboBox(this);
    m_band->setObjectName("katConfigurationBand");
    m_band->setAccessibleName(tr("Band to edit"));
    m_band->addItem(tr("All bands"), -1);
    for (int i = 0; i < 11; ++i) {
        m_band->addItem(Kat500::bandName(i), i);
    }
    actions->addWidget(m_band);
    configuration->addLayout(actions);
    label(tr("Read before editing. Double-click an Edit cell; 0/1 means off/on. All bands keeps Mixed until "
             "that field is edited. Six physical memory slots are shared among antennas. Apply verifies "
             "changed fields; Cancel edits discards only staging."),
          "katConfigurationHelp", configuration);
    m_configuration = new QTableWidget(fields.size(), 3, this);
    m_configuration->setObjectName("katConfiguration");
    m_configuration->setAccessibleName(tr("Confirmed and staged KAT500 configuration"));
    m_configuration->setHorizontalHeaderLabels({tr("Setting"), tr("Edit"), tr("Readback")});
    m_configuration->verticalHeader()->hide();
    m_configuration->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_configuration->setColumnWidth(1, 56);
    m_configuration->setColumnWidth(2, 62);
    m_configuration->verticalHeader()->setMinimumSectionSize(22);
    m_configuration->verticalHeader()->setDefaultSectionSize(22);
    m_configuration->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_configuration->setMinimumHeight(320);
    m_configuration->setWordWrap(false);
    configuration->addWidget(m_configuration);
    auto* edits = new QVBoxLayout;
    auto* apply = button(tr("Apply changed fields"), "katApplyConfiguration", edits);
    auto* discard = button(tr("Cancel edits"), "katDiscardConfiguration", edits);
    auto* optimize = button(tr("Optimize for KPA500"), "katOptimizeKpa500", edits);
    configuration->addLayout(edits);
    root->addWidget(config);
    auto* backupGroup = new QGroupBox(tr("Complete native backup and restore"), this);
    auto* backup = new QVBoxLayout(backupGroup);
    auto* files = new QVBoxLayout;
    auto* saveBackup = button(tr("Read and save backup…"), "katSaveBackup", files);
    auto* choose = button(tr("Choose backup…"), "katChooseBackup", files);
    backup->addLayout(files);
    m_preview = label(tr("No backup selected."), "katBackupPreview", backup);
    label(tr("A .KAT500Config backup contains both EEPROM regions (66,560 bytes). Restore verifies each "
             "allowed block and excludes tuner identity and fault history. Cancellation or failure cannot "
             "undo blocks already written. Manually power-cycle after a successful restore."),
          "katBackupHelp", backup);
    m_restoreConfirm =
        new QCheckBox(tr("Confirm backup replacement"), this);
    m_restoreConfirm->setObjectName("katConfirmRestore");
    backup->addWidget(m_restoreConfirm);
    label(tr("Replace this tuner's settings and stored matches with the previewed backup."),
          "katRestoreConfirmationHelp", backup);
    auto* restore = button(tr("Restore and verify"), "katRestoreBackup", backup);
    root->addWidget(backupGroup);
    auto* faultGroup = new QGroupBox(tr("Stored fault history — newest first"), this);
    auto* faults = new QVBoxLayout(faultGroup);
    auto* faultActions = new QVBoxLayout;
    auto* readFaultsButton = button(tr("Read fault history"), "katReadFaults", faultActions);
    auto* exportFaults = button(tr("Export CSV…"), "katExportFaults", faultActions);
    faults->addLayout(faultActions);
    label(
        tr("Elapsed device time is not a calendar timestamp. Power On history is distinct from current FLT0. "
           "Unknown records retain raw bytes. Clear fault in the applet clears only the current fault."),
        "katFaultHistoryHelp", faults);
    m_faults = new QTableWidget(0, 8, this);
    m_faults->setObjectName("katStoredFaults");
    m_faults->setAccessibleName(tr("Stored tuner fault history"));
    m_faults->setHorizontalHeaderLabels({tr("Slot"), tr("Elapsed s"), tr("Fault"), tr("Frequency/count"),
                                         tr("Fwd/Refl ADC"), tr("SWR"), tr("Bypass SWR"), tr("Raw bytes")});
    faults->addWidget(m_faults);
    root->addWidget(faultGroup);
    auto* memoriesGroup = new QGroupBox(tr("Memories and reset"), this);
    auto* memories = new QVBoxLayout(memoriesGroup);
    auto* memoryActions = new QVBoxLayout;
    m_memoryFrequency = new QSpinBox(this);
    m_memoryFrequency->setObjectName("katInspectFrequency");
    m_memoryFrequency->setAccessibleName(
        tr("Memory inspection frequency in kHz; zero uses current frequency"));
    m_memoryFrequency->setRange(0, 99999);
    m_memoryFrequency->setSpecialValueText(tr("Current frequency"));
    memoryActions->addWidget(m_memoryFrequency);
    auto* inspect = button(tr("Inspect bin"), "katInspectMemories", memoryActions);
    memories->addLayout(memoryActions);
    m_memorySummary = label(tr("No bin inspected."), "katMemorySummary", memories);
    m_memories = new QTableWidget(0, 6, this);
    m_memories->setObjectName("katStoredMemories");
    m_memories->setAccessibleName(tr("Stored matches, independent of live tuner state"));
    m_memories->setHorizontalHeaderLabels(
        {tr("ANT"), tr("Bypassed"), tr("C mask"), tr("L mask"), tr("C side"), tr("Bypass SWR")});
    memories->addWidget(m_memories);
    auto* eraseSelectors = new QHBoxLayout;
    m_eraseBand = new QComboBox(this);
    m_eraseBand->setObjectName("katEraseBand");
    m_eraseBand->setAccessibleName(tr("Band to erase"));
    m_eraseBand->addItem(tr("All 11 bands"), -1);
    for (int i = 0; i < 11; ++i) {
        m_eraseBand->addItem(Kat500::bandName(i), i);
    }
    eraseSelectors->addWidget(m_eraseBand);
    m_eraseAntenna = new QComboBox(this);
    m_eraseAntenna->setObjectName("katEraseAntenna");
    m_eraseAntenna->setAccessibleName(tr("Antenna memories to erase"));
    m_eraseAntenna->addItem(tr("All antennas"), 0);
    for (int i = 1; i <= 3; ++i) {
        m_eraseAntenna->addItem(tr("ANT %1").arg(i), i);
    }
    eraseSelectors->addWidget(m_eraseAntenna);
    memories->addLayout(eraseSelectors);
    auto* eraseTarget = label({}, "katEraseTarget", memories);
    m_eraseConfirm = new QCheckBox(tr("Confirm selected erase"), this);
    m_eraseConfirm->setObjectName("katConfirmErase");
    memories->addWidget(m_eraseConfirm);
    auto* erase = button(tr("Erase selected memories"), "katEraseMemories", memories);
    m_resetConfirm = new QCheckBox(
        tr("Confirm factory reset"), this);
    m_resetConfirm->setObjectName("katConfirmReset");
    memories->addWidget(m_resetConfirm);
    label(tr("Erase settings and stored matches, restart, and reconnect at the default baud."),
          "katResetConfirmationHelp", memories);
    auto* reset = button(tr("Factory reset"), "katFactoryReset", memories);
    root->addWidget(memoriesGroup);
    for (auto* table : {m_faults, m_memories}) {
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMinimumHeight(150);
        table->setMaximumHeight(230);
    }
    m_progress = new QProgressBar(this);
    m_progress->setObjectName("katMaintenanceProgress");
    m_progress->setAccessibleName(tr("Tuner operation progress"));
    root->addWidget(m_progress);
    m_status = label(tr("Ready. No operation has run."), "katMaintenanceStatus", root);
    auto* cancel = button(tr("Cancel operation"), "katCancelMaintenance", root);
    connect(m_band, &QComboBox::currentIndexChanged, this, &Kat500AdvancedWidget::renderConfiguration);
    connect(m_configuration, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() == 1) {
            stageField(item->row());
        }
    });
    connect(discard, &QPushButton::clicked, this, [this] {
        m_staged.clear();
        renderConfiguration();
        refreshAvailability();
    });
    connect(read, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_staged.clear();
            m_ownOperation = true;
            m_model->readConfiguration();
        }
    });
    connect(apply, &QPushButton::clicked, this, [this] {
        if (m_model && !m_staged.isEmpty()) {
            m_applying = true;
            m_ownOperation = true;
            m_model->applyConfiguration(m_staged);
        }
    });
    connect(optimize, &QPushButton::clicked, this, [this] {
        if (m_confirmed.isEmpty()) {
            return;
        }
        m_staged[{Kat500::Setting::AmpKeyPower, -1, 0}] = 1500;
        renderConfiguration();
        refreshAvailability();
        showStatus(tr("Staged unlimited interrupt threshold (1500). Apply changed fields to send it; this is "
                      "not a power target."));
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
        connect(check, &QCheckBox::toggled, this, &Kat500AdvancedWidget::refreshAvailability);
    }
    const auto targetChanged = [this, eraseTarget, erase] {
        m_eraseConfirm->setChecked(false);
        const QString target = tr("Erase target: %1 / %2").arg(m_eraseBand->currentText(), m_eraseAntenna->currentText());
        eraseTarget->setText(target);
        const bool all = m_eraseBand->currentData().toInt() == -1
            && m_eraseAntenna->currentData().toInt() == 0;
        erase->setText(all ? tr("Clear all ATU memories…") : tr("Erase selected memories"));
        erase->setAccessibleName(erase->text());
        m_eraseConfirm->setText(all ? tr("Confirm erasing all KAT500 matches") : tr("Confirm selected erase"));
        m_eraseConfirm->setAccessibleName(tr("Confirm selected erase. %1").arg(target));
        refreshAvailability();
    };
    connect(m_eraseBand, &QComboBox::currentIndexChanged, this, targetChanged);
    connect(m_eraseAntenna, &QComboBox::currentIndexChanged, this, targetChanged);
    targetChanged();
    if (model) {
        connect(model, &Kat500Model::configurationRead, this, &Kat500AdvancedWidget::configurationRead);
        connect(model, &Kat500Model::availabilityChanged, this, &Kat500AdvancedWidget::refreshAvailability);
        connect(model, &Kat500Model::statusUpdated, this, &Kat500AdvancedWidget::refreshAvailability);
        connect(model, &Kat500Model::operationBusyChanged, this, &Kat500AdvancedWidget::refreshAvailability);
        connect(model, &Kat500Model::connected, this, &Kat500AdvancedWidget::clearDeviceData);
        connect(model, &Kat500Model::disconnected, this, &Kat500AdvancedWidget::clearDeviceData);
        connect(model, &Kat500Model::operationProgress, this, [this](int done, int total) {
            if (m_ownOperation) {
                m_progress->setRange(0, total);
                m_progress->setValue(done);
            }
        });
        connect(model, &Kat500Model::operationFinished, this, [this](const QString& message) {
            if (m_ownOperation) {
                if (m_applying) {
                    m_staged.clear();
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
        connect(model, &Kat500Model::operationFailed, this, [this](const QString& reason) {
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
        connect(model, &Kat500Model::backupRead, this, [this](const Kat500::Backup& data) {
            if (m_savePath.isEmpty()) {
                return;
            }
            QString error;
            const QString path = m_savePath;
            m_savePath.clear();
            const bool saved = Kat500Model::saveBackupFile(path, data, &error);
            showStatus(saved ? tr("Complete backup saved: %1").arg(path)
                             : tr("Backup save failed: %1").arg(error));
        });
        connect(model, &Kat500Model::faultHistoryRead, this, &Kat500AdvancedWidget::readFaults);
        connect(model, &Kat500Model::memoriesRead, this, &Kat500AdvancedWidget::readMemories);
        connect(model, &QObject::destroyed, this, [this] {
            m_model = nullptr;
            clearDeviceData();
        });
    }
    renderConfiguration();
    refreshAvailability();
}
Kat500AdvancedWidget::~Kat500AdvancedWidget()
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
void Kat500AdvancedWidget::showStatus(const QString& message)
{
    m_status->setText(message);
    m_status->setAccessibleDescription(message);
    QAccessibleEvent event(m_status, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
QString Kat500AdvancedWidget::schemaReason(bool writing) const
{
    if (!m_model) {
        return tr("Connect a KAT500 first.");
    }
    const QString reason = m_model->unavailableReason(writing);
    if (!reason.isEmpty()) {
        return reason;
    }
    if (m_model->status().firmware != "02.16") {
        return tr("Configuration and memory schema support is verified for firmware 02.16 only. Use the "
                  "official Utility for other firmware.");
    }
    return {};
}
QString Kat500AdvancedWidget::restoreReason() const
{
    const QString reason = schemaReason(true);
    if (!reason.isEmpty()) {
        return reason;
    }
    if (!m_backup) {
        return tr("Choose a complete validated .KAT500Config backup first.");
    }
    if (m_backup->firmware != m_model->status().firmware || m_backup->serial != m_model->status().serial) {
        return tr("Backup firmware and serial must match this tuner.");
    }
    return {};
}
void Kat500AdvancedWidget::refreshAvailability()
{
    const QString readReason = schemaReason(false), writeReason = schemaReason(true);
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
         {"katReadConfiguration", "katSaveBackup", "katReadFaults", "katInspectMemories"}) {
        available(findChild<QPushButton*>(name), readReason);
    }
    available(m_configuration, !readReason.isEmpty() ? readReason
                               : m_confirmed.size() != Kat500::configurationKeys().size()
                                   ? tr("Read complete configuration before editing.")
                                   : QString());
    available(findChild<QPushButton*>("katApplyConfiguration"), !writeReason.isEmpty() ? writeReason
                                                                : m_staged.isEmpty()
                                                                    ? tr("Deliberately edit a field first.")
                                                                    : QString());
    available(findChild<QPushButton*>("katDiscardConfiguration"),
              busy ? tr("Wait for the operation to finish.") : QString());
    available(findChild<QPushButton*>("katOptimizeKpa500"),
              !writeReason.isEmpty()  ? writeReason
              : m_confirmed.isEmpty() ? tr("Read configuration before staging the KPA500 threshold.")
                                      : QString());
    available(findChild<QPushButton*>("katChooseBackup"),
              busy ? tr("Wait for the operation to finish.") : QString());
    available(m_restoreConfirm, restoreReason());
    available(findChild<QPushButton*>("katRestoreBackup"),
              !restoreReason().isEmpty()       ? restoreReason()
              : !m_restoreConfirm->isChecked() ? tr("Confirm replacement of stored settings first.")
                                               : QString());
    available(findChild<QPushButton*>("katExportFaults"), !m_faultsLoaded ? tr("Read fault history first.")
                                                          : busy ? tr("Wait for the operation to finish.")
                                                                 : QString());
    available(m_eraseConfirm, writeReason);
    available(m_resetConfirm, writeReason);
    available(findChild<QPushButton*>("katEraseMemories"),
              !writeReason.isEmpty()         ? writeReason
              : !m_eraseConfirm->isChecked() ? tr("Confirm the selected band and antenna erase target first.")
                                             : QString());
    available(findChild<QPushButton*>("katFactoryReset"),
              !writeReason.isEmpty()         ? writeReason
              : !m_resetConfirm->isChecked() ? tr("Confirm factory reset and default-baud reconnect first.")
                                             : QString());
    available(findChild<QPushButton*>("katCancelMaintenance"),
              !busy || !m_ownOperation ? tr("No operation is running from this panel.") : QString());
    m_progress->setVisible(busy && m_ownOperation);
    available(m_band, busy ? tr("Wait for the operation to finish.") : QString());
    available(m_eraseBand, busy ? tr("Wait for the operation to finish.") : QString());
    available(m_eraseAntenna, busy ? tr("Wait for the operation to finish.") : QString());
}
void Kat500AdvancedWidget::configurationRead(const Kat500::Configuration& values)
{
    m_confirmed = values;
    if (m_applying) {
        for (auto it = m_staged.begin(); it != m_staged.end();) {
            const auto setting = it.key().setting;
            const bool swr = setting == Kat500::Setting::AutoSwr || setting == Kat500::Setting::BypassSwr ||
                             setting == Kat500::Setting::KeySwr;
            if (values.contains(it.key()) &&
                std::abs(values.value(it.key()) - it.value()) <= (swr ? 0.011 : 0.0)) {
                it = m_staged.erase(it);
            } else {
                ++it;
            }
        }
    }
    renderConfiguration();
    refreshAvailability();
}
void Kat500AdvancedWidget::renderConfiguration()
{
    const QSignalBlocker block(m_configuration);
    const int band = m_band->currentData().toInt();
    for (int row = 0; row < static_cast<int>(fields.size()); ++row) {
        const Field& f = fields[row];
        QString confirmed, staged;
        std::optional<double> first, edited;
        bool mixed = false, unknown = false, mixedEdit = false;
        const int start = f.perBand ? (band == -1 ? 0 : band) : -1,
                  end = f.perBand ? (band == -1 ? 10 : band) : -1;
        for (int b = start; b <= end; ++b) {
            const Kat500::SettingKey key{f.setting, b, f.antenna};
            if (!m_confirmed.contains(key)) {
                unknown = true;
                continue;
            }
            const double value = m_confirmed.value(key);
            if (!first) {
                first = value;
            } else if (*first != value) {
                mixed = true;
            }
            const double pending = m_staged.value(key, value);
            if (!edited) {
                edited = pending;
            } else if (*edited != pending) {
                mixedEdit = true;
            }
        }
        confirmed = unknown ? tr("Unknown")
                    : mixed ? tr("Mixed")
                    : first ? QString::number(*first, 'g', 8)
                            : tr("Unknown");
        staged = unknown     ? tr("Unknown")
                 : mixedEdit ? tr("Mixed")
                 : edited    ? QString::number(*edited, 'g', 8)
                             : tr("Unknown");
        fillTable(m_configuration, row, {tr(f.title), staged, confirmed});
        m_configuration->item(row, 0)->setToolTip(tr(f.detail));
        m_configuration->item(row, 0)->setData(Qt::AccessibleDescriptionRole, tr(f.detail));
        m_configuration->item(row, 0)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_configuration->item(row, 2)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_configuration->item(row, 1)->setToolTip(
            tr("Enter %1 to %2. Mixed and Unknown are not writable values.").arg(f.minimum).arg(f.maximum));
    }
}
void Kat500AdvancedWidget::stageField(int row)
{
    if (row < 0 || row >= static_cast<int>(fields.size())) {
        return;
    }
    const Field& f = fields[row];
    bool ok = false;
    const double value = m_configuration->item(row, 1)->text().toDouble(&ok);
    const bool swr = f.setting == Kat500::Setting::AutoSwr || f.setting == Kat500::Setting::BypassSwr ||
                     f.setting == Kat500::Setting::KeySwr;
    if (!ok || !std::isfinite(value) || value < f.minimum || value > f.maximum ||
        (!swr && value != std::floor(value))) {
        showStatus(tr("Invalid value for %1; enter %2–%3.").arg(tr(f.title)).arg(f.minimum).arg(f.maximum));
        renderConfiguration();
        return;
    }
    const int band = m_band->currentData().toInt();
    const int start = f.perBand ? (band == -1 ? 0 : band) : -1,
              end = f.perBand ? (band == -1 ? 10 : band) : -1;
    for (int b = start; b <= end; ++b) {
        const Kat500::SettingKey key{f.setting, b, f.antenna};
        if (!m_confirmed.contains(key)) {
            continue;
        }
        if (m_confirmed.value(key) == value) {
            m_staged.remove(key);
        } else {
            m_staged[key] = value;
        }
    }
    showStatus(tr("%1 changed fields staged. Nothing has been sent.").arg(m_staged.size()));
    refreshAvailability();
}
void Kat500AdvancedWidget::clearDeviceData()
{
    m_confirmed.clear();
    m_staged.clear();
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
void Kat500AdvancedWidget::openFileDialog(FileAction action)
{
    if (m_fileDialog) {
        m_fileDialog->raise();
        m_fileDialog->activateWindow();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName("katMaintenanceFileDialog");
    dialog->setAccessibleName(tr("KAT500 maintenance file selection"));
    dialog->setWindowTitle(action == FileAction::Choose   ? tr("Choose KAT500 backup")
                           : action == FileAction::Backup ? tr("Save complete KAT500 backup")
                                                          : tr("Export fault history"));
    dialog->setNameFilter(action == FileAction::Faults
                              ? tr("CSV files (*.csv)")
                              : tr("KAT500 configuration (*.KAT500Config);;All files (*)"));
    dialog->setAcceptMode(action == FileAction::Choose ? QFileDialog::AcceptOpen : QFileDialog::AcceptSave);
    dialog->setFileMode(action == FileAction::Choose ? QFileDialog::ExistingFile : QFileDialog::AnyFile);
    dialog->setDefaultSuffix(action == FileAction::Faults ? "csv" : "KAT500Config");
    if (action != FileAction::Choose) {
        dialog->selectFile(
            action == FileAction::Faults
                ? "kat500-fault-history.csv"
                : QStringLiteral("SN%1.KAT500Config").arg(m_model ? m_model->status().serial : QString()));
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
            const bool saved = save(paths.first(), Kat500Model::faultsCsv(m_faultRecords), &error);
            showStatus(saved ? tr("Fault history exported.") : error);
        }
    });
    dialog->open();
}
bool Kat500AdvancedWidget::loadBackup(const QString& path)
{
    m_backup.reset();
    m_restoreConfirm->setChecked(false);
    QFile file(path);
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
    } else if (file.size() > 256 * 1024) {
        error = tr("Backup exceeds the 256 KiB limit.");
    } else {
        m_backup = Kat500Model::decodeBackupFile(file.read(256 * 1024 + 1), &error);
    }
    m_preview->setText(m_backup ? tr("%1\nS/N %2 · Firmware %3 · 66,560 bytes · complete coverage and CRC "
                                     "verified\nRestore excludes identity and fault history.")
                                      .arg(QFileInfo(path).fileName(), m_backup->serial, m_backup->firmware)
                                : tr("No valid backup selected."));
    showStatus(m_backup ? tr("Backup validated for preview. No tuner settings changed.") : error);
    refreshAvailability();
    return m_backup.has_value();
}
void Kat500AdvancedWidget::readFaults(const QList<Kat500::FaultRecord>& records)
{
    m_faultRecords = records;
    m_faultsLoaded = true;
    m_faults->setRowCount(records.size());
    for (int i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        fillTable(m_faults, i,
                  {QString::number(r.slot), QString::number(r.elapsedSeconds),
                   tr("%1: %2").arg(r.code).arg(r.description), QString::number(r.frequencyOrCount),
                   tr("%1 / %2").arg(r.forwardAdc).arg(r.reflectedAdc), QString::number(r.swr, 'f', 2),
                   QString::number(r.bypassSwr, 'f', 2), QString::fromLatin1(r.raw.toHex(' '))});
    }
    refreshAvailability();
}
void Kat500AdvancedWidget::readMemories(const Kat500::MemoryBin& bin)
{
    m_memorySummary->setText(
        tr("%1–%2 kHz · %3 matches · %4 unused slots. These rows do not change live antenna or relays.")
            .arg(bin.firstKhz)
            .arg(bin.lastKhz)
            .arg(bin.matches.size())
            .arg(bin.unused));
    m_memories->setRowCount(bin.matches.size());
    for (int i = 0; i < bin.matches.size(); ++i) {
        const auto& m = bin.matches[i];
        fillTable(m_memories, i,
                  {QString::number(m.antenna), m.bypassed ? tr("Yes") : tr("No"),
                   m.capacitorMask ? QString::number(*m.capacitorMask) : QStringLiteral("—"),
                   m.inductorMask ? QString::number(*m.inductorMask) : QStringLiteral("—"),
                   m.side ? (*m.side == Kat500::CapacitorSide::Antenna ? tr("Antenna") : tr("Transmitter"))
                          : QStringLiteral("—"),
                   QString::number(m.bypassSwr, 'f', 2)});
    }
}
} // namespace AetherSDR
