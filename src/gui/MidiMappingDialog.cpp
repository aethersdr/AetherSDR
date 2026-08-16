#include "core/ThemeManager.h"
#ifdef HAVE_MIDI

#include "MidiMappingDialog.h"
#include "core/AppSettings.h"
#include "FramelessMessageBox.h"
#include "core/MidiControlManager.h"
#include "core/MidiSettings.h"

#include <QTimer>
#include <memory>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QCheckBox>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>

namespace AetherSDR {

namespace {

// Remembered directory for profile Import/Export (mirrors the shortcut
// dialog's transfer-directory helpers).
QString midiTransferDirectory()
{
    const QString saved = AppSettings::instance()
                              .value(QStringLiteral("MidiImportExportPath"), QString())
                              .toString();
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs.isEmpty() ? QDir::homePath() : docs;
}

void rememberMidiTransferDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (!info.absolutePath().isEmpty()) {
        AppSettings::instance().setValue(QStringLiteral("MidiImportExportPath"),
                                         info.absolutePath());
    }
}

} // namespace

static const QString kGroupStyle =
    "QGroupBox { border: 1px solid #304050; border-radius: 4px; "
    "margin-top: 8px; padding-top: 12px; font-weight: bold; color: #8aa8c0; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }";

static const QString kComboStyle =
    "QComboBox { background: #1a2a3a; border: 1px solid #304050; "
    "border-radius: 3px; color: #c8d8e8; font-size: 11px; padding: 2px 6px; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; "
    "selection-background-color: #00b4d8; }";

static const QString kBtnStyle =
    "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
    "border: 1px solid #008ba8; padding: 5px 14px; border-radius: 3px; }"
    "QPushButton:hover { background: #00c8f0; }"
    "QPushButton:disabled { background: #404060; color: #808080; }";

// Muted sibling of kBtnStyle for secondary actions — Learn stays the visually
// primary way to create bindings; manual entry is the bypass (#4760).
// {{token}} template: apply via ThemeManager::applyStyleSheet, never setStyleSheet.
static const QString kBtnSubtleStyle =
    "QPushButton { background: {{color.background.1}}; color: {{color.text.primary}}; "
    "border: 1px solid {{color.border.strong}}; padding: 5px 14px; border-radius: 3px; }"
    "QPushButton:hover { background: {{color.background.2}}; color: {{color.text.primary}}; }";

// Same palette for the fixed-size square glyph buttons in the table rows. The
// 14 px side padding of kBtnSubtleStyle exceeds a 24 px button's width, which
// leaves no room for the glyph and renders it blank.
static const QString kBtnGlyphStyle =
    "QPushButton { background: {{color.background.1}}; color: {{color.text.primary}}; "
    "border: 1px solid {{color.border.strong}}; padding: 0; border-radius: 3px; }"
    "QPushButton:hover { background: {{color.background.2}}; color: {{color.text.primary}}; }";

// One apply site for the shared primary-button sheet — the colour ratchet
// counts setStyleSheet() call sites, not colours (tools/audit_colours.py).
static QPushButton* makeStyledButton(const QString& text)
{
    auto* btn = new QPushButton(text);
    btn->setStyleSheet(kBtnStyle);
    return btn;
}

MidiMappingDialog::MidiMappingDialog(MidiControlManager* manager, QWidget* parent)
    : PersistentDialog("MIDI Controller Mapping", "MidiMappingDialogGeometry", parent),
      m_manager(manager)
{
    theme::setContainer(this, QStringLiteral("dialog/midiMapping"));
    setMinimumSize(700, 550);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; }");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    // ── Device section ──────────────────────────────────────────────────
    {
        auto* group = new QGroupBox("MIDI Device");
        group->setStyleSheet(kGroupStyle);
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        grid->addWidget(new QLabel("Port:"), 0, 0);
        m_portCombo = new QComboBox;
        m_portCombo->setStyleSheet(kComboStyle);
        m_portCombo->setMinimumWidth(250);
        grid->addWidget(m_portCombo, 0, 1);

        auto* refreshBtn = makeStyledButton("Refresh");
        connect(refreshBtn, &QPushButton::clicked, this, &MidiMappingDialog::refreshPortList);
        grid->addWidget(refreshBtn, 0, 2);

        m_connectBtn = makeStyledButton("Connect");
        connect(m_connectBtn, &QPushButton::clicked, this, [this] {
            if (m_manager->isOpen()) {
                m_manager->closePort();
                m_connectBtn->setText("Connect");
                m_statusLabel->setText("Disconnected");
                AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, "QLabel { color: {{color.text.label}}; font-size: 11px; }");
            } else {
                int idx = m_portCombo->currentIndex();
                if (idx < 0) return;
                if (m_manager->openPort(idx)) {
                    m_connectBtn->setText("Disconnect");
                    m_statusLabel->setText("Connected: " + m_manager->currentPortName());
                    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, "QLabel { color: {{color.accent.success}}; font-size: 11px; }");
                    // Save device preference
                    auto& ms = MidiSettings::instance();
                    ms.setLastDevice(m_manager->currentPortName());
                    ms.save();
                }
            }
        });
        grid->addWidget(m_connectBtn, 0, 3);

        m_statusLabel = new QLabel(m_manager->isOpen()
            ? "Connected: " + m_manager->currentPortName() : "Disconnected");
        m_statusLabel->setStyleSheet(m_manager->isOpen()
            ? "QLabel { color: #30d050; font-size: 11px; }"
            : "QLabel { color: #808080; font-size: 11px; }");
        grid->addWidget(m_statusLabel, 1, 0, 1, 2);

        m_activityLabel = new QLabel("");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_activityLabel, "QLabel { color: {{color.accent}}; font-size: 10px; font-family: monospace; }");
        grid->addWidget(m_activityLabel, 1, 2, 1, 2);

        auto* autoConnect = new QCheckBox("Auto-connect on startup");
        AetherSDR::ThemeManager::instance().applyStyleSheet(autoConnect, "QCheckBox { color: {{color.text.primary}}; }");
        autoConnect->setChecked(MidiSettings::instance().autoConnect());
        connect(autoConnect, &QCheckBox::toggled, this, [](bool on) {
            MidiSettings::instance().setAutoConnect(on);
            MidiSettings::instance().save();
        });
        grid->addWidget(autoConnect, 2, 0, 1, 4);

        root->addWidget(group);
    }

    // ── Binding table ───────────────────────────────────────────────────
    {
        auto* group = new QGroupBox("Parameter Bindings");
        group->setStyleSheet(kGroupStyle);
        auto* vbox = new QVBoxLayout(group);

        m_bindingTable = new QTableWidget;
        m_bindingTable->setColumnCount(7);
        m_bindingTable->setHorizontalHeaderLabels({"Parameter", "MIDI Source", "Channel", "Invert", "Relative", "", ""});
        m_bindingTable->horizontalHeader()->setStretchLastSection(false);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
        m_bindingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_bindingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_bindingTable->verticalHeader()->setVisible(false);
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_bindingTable, "QTableWidget { background: {{color.background.0}}; color: {{color.text.primary}}; "
            "border: 1px solid {{color.background.1}}; font-size: 11px; gridline-color: {{color.background.1}}; }"
            "QTableWidget::item:selected { background: {{color.accent}}; color: {{color.background.0}}; }"
            "QHeaderView::section { background: {{color.background.0}}; color: {{color.text.secondary}}; "
            "border: 1px solid {{color.background.1}}; padding: 3px; font-size: 11px; }");
        vbox->addWidget(m_bindingTable, 1);

        // Add binding row
        auto* addRow = new QHBoxLayout;
        m_categoryCombo = new QComboBox;
        m_categoryCombo->setStyleSheet(kComboStyle);
        m_categoryCombo->addItems({"All", "RX", "TX", "Phone/CW", "EQ",
                                   "Global", "Mode", "Band", "Filter",
                                   "Slice", "Display", "Frequency"});
        addRow->addWidget(m_categoryCombo);

        m_paramCombo = new QComboBox;
        m_paramCombo->setStyleSheet(kComboStyle);
        m_paramCombo->setObjectName("midiParamCombo");
        m_paramCombo->setMinimumWidth(200);
        addRow->addWidget(m_paramCombo, 1);

        auto populateParams = [this]() {
            m_paramCombo->clear();
            QString cat = m_categoryCombo->currentText();
            for (const auto& p : m_manager->params()) {
                if (cat != "All" && p.category != cat) continue;
                m_paramCombo->addItem(QString("[%1] %2").arg(p.category, p.displayName), p.id);
            }
        };
        connect(m_categoryCombo, &QComboBox::currentTextChanged, this, populateParams);
        populateParams();

        auto* learnBtn = makeStyledButton("Learn");
        learnBtn->setToolTip("Add binding: select a parameter, click Learn, then move a knob on your controller");
        connect(learnBtn, &QPushButton::clicked, this, [this, learnBtn] {
            if (m_manager->isLearning()) {
                m_manager->cancelLearn();
                learnBtn->setText("Learn");
                return;
            }
            QString paramId = m_paramCombo->currentData().toString();
            if (paramId.isEmpty()) return;
            m_manager->startLearn(paramId);
            learnBtn->setText("Cancel Learn");
        });
        addRow->addWidget(learnBtn);

        connect(m_manager, &MidiControlManager::learnCompleted, this,
                [this, learnBtn](const QString&, const MidiBinding&) {
            learnBtn->setText("Learn");
            refreshBindingTable();
            // Save bindings
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        connect(m_manager, &MidiControlManager::learnCancelled, this,
                [learnBtn] { learnBtn->setText("Learn"); });

        auto* manualBtn = new QPushButton("Manual…");
        AetherSDR::ThemeManager::instance().applyStyleSheet(manualBtn, kBtnSubtleStyle);
        manualBtn->setObjectName("midiManualAddButton");
        manualBtn->setAccessibleName("Add MIDI binding manually");
        manualBtn->setToolTip("Add binding by typing channel, message type and number — "
                              "for controllers whose messages Learn cannot capture cleanly");
        connect(manualBtn, &QPushButton::clicked, this, [this] {
            QString paramId = m_paramCombo->currentData().toString();
            if (paramId.isEmpty()) return;
            // If the parameter is already bound, open as an edit — addBinding()
            // removes by paramId first, so a blank form + accidental OK would
            // silently replace the existing binding with defaults.  Pre-filling
            // makes Manual… and the row ✎ behave identically.
            for (const auto& cur : m_manager->bindings()) {
                if (cur.paramId == paramId) {
                    const MidiBinding copy = cur;
                    openManualEditor(paramId, &copy);
                    return;
                }
            }
            openManualEditor(paramId, nullptr);
        });
        addRow->addWidget(manualBtn);

        vbox->addLayout(addRow);

        // Button row
        auto* btnRow = new QHBoxLayout;
        auto* clearAllBtn = makeStyledButton("Clear All");
        connect(clearAllBtn, &QPushButton::clicked, this, [this] {
            m_manager->clearBindings();
            refreshBindingTable();
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        btnRow->addWidget(clearAllBtn);
        btnRow->addStretch();

        // Profile management
        m_profileCombo = new QComboBox;
        m_profileCombo->setStyleSheet(kComboStyle);
        m_profileCombo->setMinimumWidth(120);
        m_profileCombo->setEditable(true);
        m_profileCombo->setPlaceholderText("profile name");
        btnRow->addWidget(new QLabel("Profile:"));
        btnRow->addWidget(m_profileCombo);

        auto* saveProfileBtn = makeStyledButton("Save");
        saveProfileBtn->setToolTip(
            QStringLiteral("Save the current bindings as a named profile"));
        connect(saveProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;
            MidiSettings::instance().saveProfile(name, m_manager->bindings());
            refreshProfileList();
        });
        btnRow->addWidget(saveProfileBtn);

        auto* loadProfileBtn = makeStyledButton("Load");
        loadProfileBtn->setToolTip(
            QStringLiteral("Apply the selected profile to the current bindings"));
        connect(loadProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;
            auto bindings = MidiSettings::instance().loadProfile(name);
            if (bindings.isEmpty()) {
                // A missing or empty profile silently doing nothing is the
                // same failure class the importer refuses — say so instead.
                FramelessMessageBox::warning(
                    this, QStringLiteral("Load Profile"),
                    QStringLiteral("Profile \"%1\" is empty or missing.").arg(name));
                return;
            }
            m_manager->clearBindings();
            for (const auto& b : bindings)
                m_manager->addBinding(b);
            refreshBindingTable();
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        btnRow->addWidget(loadProfileBtn);

        auto* importProfileBtn = makeStyledButton("Import...");
        importProfileBtn->setObjectName(QStringLiteral("midiProfileImportButton"));
        importProfileBtn->setAccessibleName(QStringLiteral("Import MIDI profile"));
        importProfileBtn->setToolTip(QStringLiteral(
            "Import a profile file into the store — AetherSDR profile XML or SmartSDR \".map\""));
        connect(importProfileBtn, &QPushButton::clicked, this,
                &MidiMappingDialog::importProfileFromFile);
        btnRow->addWidget(importProfileBtn);

        auto* exportProfileBtn = makeStyledButton("Export...");
        exportProfileBtn->setObjectName(QStringLiteral("midiProfileExportButton"));
        exportProfileBtn->setAccessibleName(QStringLiteral("Export MIDI profile"));
        exportProfileBtn->setToolTip(
            QStringLiteral("Export the current bindings as an AetherSDR profile XML"));
        connect(exportProfileBtn, &QPushButton::clicked, this,
                &MidiMappingDialog::exportProfileToFile);
        btnRow->addWidget(exportProfileBtn);

        vbox->addLayout(btnRow);
        root->addWidget(group, 1);
    }

    // ── Close button ────────────────────────────────────────────────────
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = makeStyledButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // ── Activity indicator ──────────────────────────────────────────────
    connect(m_manager, &MidiControlManager::midiActivity, this,
            [this](int channel, int type, int number, int value) {
        static const char* typeNames[] = {"CC", "Note", "NoteOff", "PBend"};
        const char* tn = (type >= 0 && type <= 3) ? typeNames[type] : "?";
        m_activityLabel->setText(QString("Ch %1 %2 #%3 = %4")
            .arg(channel + 1).arg(tn).arg(number).arg(value));
    });

    // Initial state
    refreshPortList();
    refreshBindingTable();
    refreshProfileList();

    if (m_manager->isOpen()) {
        m_connectBtn->setText("Disconnect");
    }
}

void MidiMappingDialog::importProfileFromFile()
{
    QFileDialog dialog(this, QStringLiteral("Import MIDI Profile"),
                       midiTransferDirectory(),
                       QStringLiteral("MIDI profiles (*.xml *.map);;All files (*)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    const QString path = dialog.selectedFiles().first();
    rememberMidiTransferDirectory(path);

    const MidiImportResult result = MidiSettings::instance().importProfile(
        path,
        [this](const QString& id) { return m_manager->findParam(id) != nullptr; });

    const QString fileName = QFileInfo(path).fileName();
    if (!result.ok()) {
        FramelessMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Import MIDI Profile"));
        box.setText(QStringLiteral("No bindings were imported from %1.").arg(fileName));
        box.setStandardButtons(QMessageBox::Ok);
        box.setDetailedText(result.errors.join(QLatin1Char('\n')));
        box.exec();
        return;
    }

    // Layered report, like the shortcut importer: headline count up front,
    // per-category counts as informative text, every skipped name in the
    // expandable details — import what maps, name what doesn't.
    QStringList informativeLines;
    QStringList detailLines;
    const auto addSection = [&](const QString& header, const QStringList& names,
                                const QString& summary) {
        if (names.isEmpty())
            return;
        informativeLines << summary.arg(names.size());
        if (!detailLines.isEmpty())
            detailLines << QString();
        detailLines << header;
        detailLines << names;
    };
    addSection(QStringLiteral("Skipped (no matching AetherSDR control):"),
               result.skippedUnknownParam,
               QStringLiteral("%1 control(s) have no AetherSDR equivalent and were skipped."));
    addSection(QStringLiteral("Skipped (invalid values):"), result.skippedBadType,
               QStringLiteral("%1 row(s) had out-of-range values and were skipped."));
    addSection(QStringLiteral("Dropped duplicates:"), result.duplicates,
               QStringLiteral("%1 duplicate row(s) were dropped."));

    if (result.importedCount == 0) {
        FramelessMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Import MIDI Profile"));
        box.setText(QStringLiteral("No usable bindings in %1.").arg(fileName));
        box.setStandardButtons(QMessageBox::Ok);
        if (!informativeLines.isEmpty())
            box.setInformativeText(informativeLines.join(QLatin1Char('\n')));
        if (!detailLines.isEmpty())
            box.setDetailedText(detailLines.join(QLatin1Char('\n')));
        box.exec();
        return;
    }

    refreshProfileList();
    m_profileCombo->setCurrentText(result.profileName);

    FramelessMessageBox box(this);
    box.setIcon(informativeLines.isEmpty() ? QMessageBox::Information
                                           : QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Import MIDI Profile"));
    box.setText(QStringLiteral(
                    "Imported %1 binding(s) from %2 as profile \"%3\". "
                    "Click Load to apply it.")
                    .arg(result.importedCount)
                    .arg(fileName, result.profileName));
    box.setStandardButtons(QMessageBox::Ok);
    if (!informativeLines.isEmpty())
        box.setInformativeText(informativeLines.join(QLatin1Char('\n')));
    if (!detailLines.isEmpty())
        box.setDetailedText(detailLines.join(QLatin1Char('\n')));
    box.exec();
}

void MidiMappingDialog::exportProfileToFile()
{
    const auto& bindings = m_manager->bindings();
    if (bindings.isEmpty()) {
        FramelessMessageBox::information(this, QStringLiteral("Export MIDI Profile"),
                                         QStringLiteral("There are no bindings to export."));
        return;
    }

    // yyyyMMdd_HHmmss so two exports the same day don't suggest the identical
    // filename (matches the shortcut exporter).
    const QString fileName = QStringLiteral("AetherSDR_MidiProfile_%1_v%2.xml")
                                 .arg(QDateTime::currentDateTime().toString(
                                          QStringLiteral("yyyyMMdd_HHmmss")),
                                      QCoreApplication::applicationVersion());
    QFileDialog dialog(this, QStringLiteral("Export MIDI Profile"),
                       QDir(midiTransferDirectory()).filePath(fileName),
                       QStringLiteral("MIDI profile XML (*.xml)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("xml"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    const QString path = dialog.selectedFiles().first();
    rememberMidiTransferDirectory(path);

    const MidiExportResult result = MidiSettings::instance().exportProfile(path, bindings);
    if (!result.ok()) {
        FramelessMessageBox::warning(this, QStringLiteral("Export MIDI Profile"),
                                     result.error);
        return;
    }
    FramelessMessageBox::information(
        this, QStringLiteral("Export MIDI Profile"),
        QStringLiteral("Exported %1 binding(s) to %2.")
            .arg(result.exportedCount)
            .arg(QFileInfo(path).fileName()));
}

void MidiMappingDialog::refreshPortList()
{
    m_portCombo->clear();
    for (const auto& port : m_manager->availablePorts())
        m_portCombo->addItem(port);
}

void MidiMappingDialog::refreshBindingTable()
{
    const auto& bindings = m_manager->bindings();
    m_bindingTable->setRowCount(bindings.size());

    for (int i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];

        // Parameter name
        const MidiParam* p = m_manager->findParam(b.paramId);
        QString paramName = p ? QString("[%1] %2").arg(p->category, p->displayName) : b.paramId;
        m_bindingTable->setItem(i, 0, new QTableWidgetItem(paramName));

        // MIDI source
        m_bindingTable->setItem(i, 1, new QTableWidgetItem(b.sourceDisplayName()));

        // Channel
        m_bindingTable->setItem(i, 2, new QTableWidgetItem(
            b.channel >= 0 ? QString::number(b.channel + 1) : "Any"));

        // Invert checkbox
        auto* invertCheck = new QCheckBox;
        invertCheck->setChecked(b.inverted);
        invertCheck->setStyleSheet("QCheckBox { padding-left: 10px; }");
        connect(invertCheck, &QCheckBox::toggled, this, [this, i](bool on) {
            if (i < m_manager->bindings().size()) {
                m_manager->bindings()[i].inverted = on;
                m_manager->rebuildIndex();
                MidiSettings::instance().saveBindings(m_manager->bindings());
            }
        });
        m_bindingTable->setCellWidget(i, 3, invertCheck);

        // Relative checkbox (CC sends delta, not absolute position)
        auto* relCheck = new QCheckBox;
        relCheck->setChecked(b.relative);
        relCheck->setStyleSheet("QCheckBox { padding-left: 10px; }");
        connect(relCheck, &QCheckBox::toggled, this, [this, i](bool on) {
            if (i < m_manager->bindings().size()) {
                m_manager->bindings()[i].relative = on;
                m_manager->rebuildIndex();
                MidiSettings::instance().saveBindings(m_manager->bindings());
            }
        });
        m_bindingTable->setCellWidget(i, 4, relCheck);

        // Edit button — manual correction of this binding's source (#4760)
        auto* editBtn = new QPushButton("✎");
        editBtn->setFixedSize(24, 24);
        AetherSDR::ThemeManager::instance().applyStyleSheet(editBtn, kBtnGlyphStyle);
        editBtn->setToolTip("Edit this binding's channel, type and number manually");
        editBtn->setAccessibleName(QString("Edit binding for %1").arg(paramName));
        connect(editBtn, &QPushButton::clicked, this, [this, paramId = b.paramId] {
            // Deferred: openManualEditor() ends in refreshBindingTable(),
            // which destroys this very button while its clicked emission is
            // still on the stack — get off it before opening the nested
            // modal.  (Same pattern the × button relies on implicitly.)
            QTimer::singleShot(0, this, [this, paramId] {
                // Look the binding up fresh at open time — the captured row
                // index could go stale after any table mutation.  Copy before
                // opening: committing mutates the vector the reference points
                // into.
                for (const auto& cur : m_manager->bindings()) {
                    if (cur.paramId == paramId) {
                        const MidiBinding copy = cur;
                        openManualEditor(paramId, &copy);
                        return;
                    }
                }
            });
        });
        m_bindingTable->setCellWidget(i, 5, editBtn);

        // Delete button
        auto* delBtn = new QPushButton("×");
        delBtn->setFixedSize(24, 24);
        delBtn->setStyleSheet(
            "QPushButton { background: #602020; color: #ff6060; font-weight: bold; "
            "border: 1px solid #803030; border-radius: 3px; }");
        connect(delBtn, &QPushButton::clicked, this, [this, paramId = b.paramId] {
            m_manager->removeBinding(paramId);
            refreshBindingTable();
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        m_bindingTable->setCellWidget(i, 6, delBtn);
    }
}

void MidiMappingDialog::refreshProfileList()
{
    QString current = m_profileCombo->currentText();
    m_profileCombo->clear();
    for (const auto& name : MidiSettings::instance().availableProfiles())
        m_profileCombo->addItem(name);
    if (!current.isEmpty())
        m_profileCombo->setCurrentText(current);
}

void MidiMappingDialog::openManualEditor(const QString& paramId, const MidiBinding* existing)
{
    // A stray controller touch must not complete a half-armed Learn while the
    // operator is typing in this form.
    if (m_manager->isLearning())
        m_manager->cancelLearn();

    const MidiParam* param = m_manager->findParam(paramId);
    const QString paramLabel = param
        ? QString("[%1] %2").arg(param->category, param->displayName) : paramId;
    // Learn forces relative=true on VFO CC captures (onMidiMessage); the form
    // mirrors that as a default so a typed VFO knob behaves like a learned one.
    const bool isVfoKnob = MidiControlManager::isVfoTuneKnob(paramId);

    QDialog dlg(this);
    dlg.setWindowTitle(existing ? "Edit MIDI Binding" : "Add MIDI Binding");
    dlg.setModal(true);
    dlg.setObjectName("midiManualBindingDialog");
    // checkBoxIndicatorStyle() is the #4013 fix for invisible indicators in
    // dark themes; it returns an unresolved template, so it must ride through
    // applyStyleSheet (not setStyleSheet) like the dialogs it came from.
    AetherSDR::ThemeManager::instance().applyStyleSheet(&dlg,
        QString("QDialog { background: {{color.background.0}}; }"
        "QLabel { color: {{color.text.primary}}; }"
        "QCheckBox { color: {{color.text.primary}}; }"
        "QComboBox, QSpinBox { background: {{color.background.1}}; "
        "border: 1px solid {{color.border.strong}}; border-radius: 3px; "
        "color: {{color.text.primary}}; font-size: 11px; padding: 2px 6px; }")
        + ThemeManager::checkBoxIndicatorStyle());

    auto* form = new QFormLayout(&dlg);
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(8);

    form->addRow(new QLabel(QString("Binding for: %1").arg(paramLabel)));

    auto* channelCombo = new QComboBox(&dlg);
    channelCombo->setObjectName("manualChannelCombo");
    channelCombo->setAccessibleName("MIDI channel");
    channelCombo->addItem("Any", -1);
    for (int ch = 1; ch <= 16; ++ch)
        channelCombo->addItem(QString::number(ch), ch - 1);
    // Default to channel 1, the overwhelmingly common case; "Any" stays one
    // click away. (Learn can only ever produce channel-specific bindings, so
    // this form is the first UI able to reach the supported -1 wildcard.)
    channelCombo->setCurrentIndex(1);
    form->addRow("Channel:", channelCombo);

    auto* typeCombo = new QComboBox(&dlg);
    typeCombo->setObjectName("manualTypeCombo");
    typeCombo->setAccessibleName("MIDI message type");
    typeCombo->addItem("Note On", int(MidiBinding::NoteOn));
    typeCombo->addItem("Control Change (CC)", int(MidiBinding::CC));
    typeCombo->addItem("Pitch Bend", int(MidiBinding::PitchBend));
    // The menu omits Note Off: Learn never creates NoteOff bindings, and
    // dispatch already routes NoteOff to the matching NoteOn binding for Gate
    // params — offering it would invite dead bindings. An existing NoteOff row
    // (possible in an old settings file) must still display truthfully, though.
    if (existing && existing->msgType == MidiBinding::NoteOff)
        typeCombo->addItem("Note Off", int(MidiBinding::NoteOff));
    form->addRow("Type:", typeCombo);

    auto* numberSpin = new QSpinBox(&dlg);
    numberSpin->setRange(0, 127);
    numberSpin->setObjectName("manualNumberSpin");
    numberSpin->setAccessibleName("Note or CC number");
    form->addRow("Number:", numberSpin);

    auto* invertCheck = new QCheckBox("Invert value range", &dlg);
    invertCheck->setObjectName("manualInvertCheck");
    form->addRow(QString(), invertCheck);

    auto* relativeCheck = new QCheckBox("Relative (knob sends deltas)", &dlg);
    relativeCheck->setObjectName("manualRelativeCheck");
    form->addRow(QString(), relativeCheck);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(box);

    if (existing) {
        const int chIdx = channelCombo->findData(existing->channel);
        if (chIdx >= 0) channelCombo->setCurrentIndex(chIdx);
        const int typeIdx = typeCombo->findData(int(existing->msgType));
        if (typeIdx >= 0) typeCombo->setCurrentIndex(typeIdx);
        numberSpin->setValue(qBound(0, existing->number, 127));
        invertCheck->setChecked(existing->inverted);
        relativeCheck->setChecked(existing->relative);
    }

    // Remember the Relative intent across type round-trips: without this, an
    // existing binding's Relative flag (or a deliberate mid-dialog uncheck)
    // was silently lost by flipping Type away from CC and back — the box
    // would come back at the default instead of the user's state.
    const auto lastCcRelative =
        std::make_shared<bool>(existing ? existing->relative : isVfoKnob);
    const auto prevWasCc = std::make_shared<bool>(false);
    auto refreshFieldStates =
        [typeCombo, numberSpin, relativeCheck, lastCcRelative, prevWasCc] {
        const auto t = MidiBinding::MsgType(typeCombo->currentData().toInt());
        numberSpin->setEnabled(t != MidiBinding::PitchBend);   // PB carries no number
        const bool isCc = (t == MidiBinding::CC);
        if (*prevWasCc && !isCc)
            *lastCcRelative = relativeCheck->isChecked();  // leaving CC — keep intent
        relativeCheck->setEnabled(isCc);                   // dispatch honors it for CC only
        // Non-CC never saves relative — keep the display equal to what OK
        // would commit; back on CC, restore the remembered intent (the VFO
        // Learn-mirror default for a fresh binding, the stored flag for an
        // existing one, or whatever the user last set).
        relativeCheck->setChecked(isCc ? *lastCcRelative : false);
        *prevWasCc = isCc;
    };
    connect(typeCombo, &QComboBox::currentIndexChanged, &dlg, refreshFieldStates);
    refreshFieldStates();

    if (dlg.exec() != QDialog::Accepted)
        return;

    MidiBinding b;
    b.channel  = channelCombo->currentData().toInt();
    b.msgType  = MidiBinding::MsgType(typeCombo->currentData().toInt());
    // Learn stores -1 for Pitch Bend (the message carries no number); mirror it.
    b.number   = (b.msgType == MidiBinding::PitchBend) ? -1 : numberSpin->value();
    b.paramId  = paramId;
    b.inverted = invertCheck->isChecked();
    b.relative = relativeCheck->isChecked() && b.msgType == MidiBinding::CC;

    // Duplicate-source guard. The dispatch index is keyed on (channel, type,
    // number) without the param, so on a collision the last table row silently
    // wins and the older binding stops responding with no indication. Learn
    // rarely produces this; a typed form is one typo away. Never shadow
    // silently — name the loser and ask.
    QStringList shadowedNames;
    QStringList shadowedIds;
    for (const auto& cur : m_manager->bindings()) {
        // Overlap, not key() equality: key() folds the wildcard channel to
        // 0xFF while dispatch probes the exact-channel key first and falls
        // back to the wildcard — so an "Any" binding and a channel-specific
        // one on the same type+number collide at runtime with different
        // keys, and this form is the only UI able to create the wildcard.
        // (A legacy NoteOff row shadowing the NoteOn pairing fallback for a
        // Gate param is a separate, far rarer case — out of scope here.)
        const bool sameSource = cur.msgType == b.msgType
            && (b.msgType == MidiBinding::PitchBend || cur.number == b.number)
            && (cur.channel < 0 || b.channel < 0 || cur.channel == b.channel);
        if (cur.paramId != b.paramId && sameSource) {
            const MidiParam* cp = m_manager->findParam(cur.paramId);
            shadowedNames << (cp ? QString("[%1] %2").arg(cp->category, cp->displayName)
                                 : cur.paramId);
            shadowedIds << cur.paramId;
        }
    }
    if (!shadowedIds.isEmpty()) {
        const auto answer = FramelessMessageBox::question(
            this, "Source Already Bound",
            QString("%1 is already bound to:\n  %2\n\n"
                    "Two bindings on the same or overlapping source cannot "
                    "both respond on the channel they share — the older one "
                    "would silently stop working there.\n\n"
                    "Replace the existing binding%3?")
                .arg(b.sourceDisplayName(), shadowedNames.join("\n  "),
                     shadowedIds.size() > 1 ? QStringLiteral("s") : QString()));
        if (answer != FramelessMessageBox::Yes)
            return;
        for (const auto& id : shadowedIds)
            m_manager->removeBinding(id);
    }

    m_manager->addBinding(b);
    refreshBindingTable();
    MidiSettings::instance().saveBindings(m_manager->bindings());
}

} // namespace AetherSDR

#endif // HAVE_MIDI
