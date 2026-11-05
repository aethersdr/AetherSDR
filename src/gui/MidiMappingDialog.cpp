#include "core/ThemeManager.h"
#ifdef HAVE_MIDI

#include "MidiMappingDialog.h"
#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "CanonScrollBarStyle.h"
#include "CanonTableStyle.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"
#include "core/AppSettings.h"
#include "ScopedChildWidget.h"
#include "core/MidiControlManager.h"
#include "core/MidiSettings.h"

#include <QTimer>
#include <memory>

#include <QAbstractItemView>
#include <QHideEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFormLayout>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
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

// What the shared canon dialog sheet (ModemChrome::Look::Canon: buttons,
// fields, combo boxes, spin boxes, check boxes) leaves out, after the canon
// table rules: group boxes as MQTT Settings has them; the table's selection
// ink; the combo lists; Learn outlined as the primary way to add a binding
// (#4760); and the fields' right-click menus, children of the fields that the
// sheet's transparent rule would otherwise leave with no panel.
constexpr const char* kMidiMappingRules = R"(
QGroupBox {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    margin-top: 14px;
    padding-top: 6px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: {{color.canon.cyan}};
}
QTableView::item:selected {
    color: {{color.canon.ink}};
}
QLineEdit,
QSpinBox {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QComboBox QAbstractItemView {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.lineHi}};
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QPushButton#midiLearnButton {
    color: {{color.canon.cyan}};
    border-color: {{color.canon.cyan}};
}
QLabel#midiActivity {
    color: {{color.canon.cyan}};
    font-size: 10px;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
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

QString midiMappingSheet()
{
    return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
         + canonTableRules() + canonScrollBarRules() + canonSpinArrowRules()
         + QString::fromLatin1(kMidiMappingRules);
}

// The fixed-size square glyph buttons in the table rows. The shared sheet's
// button padding exceeds a 24 px button's width and would leave no room for
// the glyph, so they carry their own. The delete glyph is drawn as destructive,
// in the gold CanonMessageBox gives a destructive button.
// {{token}} templates: apply via ThemeManager::applyStyleSheet.
static const QString kBtnGlyphStyle =
    "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}}; "
    "border: 1px solid {{color.canon.lineHi}}; padding: 0; border-radius: 4px; }"
    "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }";
static const QString kBtnGlyphDestructiveStyle =
    "QPushButton { background: {{color.canon.control}}; color: {{color.canon.sparkGold}}; "
    "border: 1px solid {{color.canon.lineHi}}; padding: 0; border-radius: 4px; font-weight: bold; }"
    "QPushButton:hover { background: {{color.canon.nested}}; }";

MidiMappingDialog::MidiMappingDialog(MidiControlManager* manager, QWidget* parent)
    : CanonWindow(QStringLiteral("MIDI Controller Mapping"), parent, Kind::Workspace),
      m_manager(manager)
{
    setObjectName(QStringLiteral("MidiMappingDialog"));
    theme::setContainer(this, QStringLiteral("canon/midiMapping"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("MidiMappingDialogGeometry"));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] { return midiMappingSheet(); });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    root->addWidget(makeCanonHeader(QStringLiteral("MIDI Controller Mapping")));

    // ── Device section ──────────────────────────────────────────────────
    {
        auto* group = new QGroupBox("MIDI Device");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        grid->addWidget(new QLabel("Port:"), 0, 0);
        m_portCombo = new QComboBox;
        m_portCombo->setObjectName(QStringLiteral("midiPortCombo"));
        m_portCombo->setMinimumWidth(250);
        grid->addWidget(m_portCombo, 0, 1);

        auto* refreshBtn = new QPushButton("Refresh");
        connect(refreshBtn, &QPushButton::clicked, this, &MidiMappingDialog::refreshPortList);
        grid->addWidget(refreshBtn, 0, 2);

        m_connectBtn = new QPushButton("Connect");
        m_connectBtn->setObjectName(QStringLiteral("midiConnectButton"));
        connect(m_connectBtn, &QPushButton::clicked, this, [this] {
            if (m_manager->isOpen()) {
                m_manager->closePort();
                m_connectBtn->setText("Connect");
                setPortStatus(QStringLiteral("Disconnected"),
                              QStringLiteral("{{color.text.label}}"));
            } else {
                connectToSelectedPort();
            }
        });
        grid->addWidget(m_connectBtn, 0, 3);

        m_statusLabel = new QLabel;
        m_statusLabel->setObjectName(QStringLiteral("midiPortStatus"));
        m_statusLabel->setTextFormat(Qt::PlainText);
        setPortStatus(m_manager->isOpen() ? "Connected: " + m_manager->currentPortName()
                                          : QStringLiteral("Disconnected"),
                      m_manager->isOpen() ? QStringLiteral("{{color.accent.success}}")
                                          : QStringLiteral("{{color.text.label}}"));
        grid->addWidget(m_statusLabel, 1, 0, 1, 2);

        m_activityLabel = new QLabel("");
        m_activityLabel->setObjectName(QStringLiteral("midiActivity"));
        grid->addWidget(m_activityLabel, 1, 2, 1, 2);

        auto* autoConnect = new QCheckBox("Auto-connect on startup");
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
        auto* vbox = new QVBoxLayout(group);

        m_bindingTable = new QTableWidget;
        m_bindingTable->setObjectName(QStringLiteral("midiBindingTable"));
        m_bindingTable->setAccessibleName(QStringLiteral("MIDI parameter bindings"));
        m_bindingTable->setColumnCount(7);
        m_bindingTable->setHorizontalHeaderLabels({"Parameter", "MIDI Source", "Channel", "Invert", "Relative", "", ""});
        m_bindingTable->horizontalHeader()->setStretchLastSection(false);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        // The edit and delete columns hold 24 px glyph buttons and no text,
        // so sizing them to contents measures the empty header and clips the
        // button; give them room for it.
        for (int glyphColumn : {5, 6}) {
            m_bindingTable->horizontalHeader()->setSectionResizeMode(glyphColumn, QHeaderView::Fixed);
            m_bindingTable->horizontalHeader()->resizeSection(glyphColumn, 34);
        }
        m_bindingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_bindingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_bindingTable->verticalHeader()->setVisible(false);
        vbox->addWidget(m_bindingTable, 1);

        // Add binding row
        auto* addRow = new QHBoxLayout;
        m_categoryCombo = new QComboBox;
        m_categoryCombo->setObjectName(QStringLiteral("midiCategoryCombo"));
        m_categoryCombo->addItems({"All", "RX", "TX", "Phone/CW", "EQ",
                                   "Global", "Mode", "Band", "Filter",
                                   "Slice", "Display", "Frequency"});
        addRow->addWidget(m_categoryCombo);

        m_paramCombo = new QComboBox;
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

        auto* learnBtn = new QPushButton("Learn");
        learnBtn->setObjectName(QStringLiteral("midiLearnButton"));
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
        auto* clearAllBtn = new QPushButton("Clear All");
        clearAllBtn->setObjectName(QStringLiteral("midiClearAllButton"));
        connect(clearAllBtn, &QPushButton::clicked, this, [this] {
            m_manager->clearBindings();
            refreshBindingTable();
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        btnRow->addWidget(clearAllBtn);
        btnRow->addStretch();

        // Profile management
        m_profileCombo = new QComboBox;
        m_profileCombo->setObjectName(QStringLiteral("midiProfileCombo"));
        m_profileCombo->setAccessibleName(QStringLiteral("MIDI profile name"));
        m_profileCombo->setMinimumWidth(120);
        m_profileCombo->setEditable(true);
        m_profileCombo->setPlaceholderText("profile name");
        btnRow->addWidget(new QLabel("Profile:"));
        btnRow->addWidget(m_profileCombo);

        auto* saveProfileBtn = new QPushButton("Save");
        saveProfileBtn->setObjectName(QStringLiteral("midiProfileSaveButton"));
        saveProfileBtn->setAccessibleName(QStringLiteral("Save MIDI profile"));
        // No name means no target: show that before the click rather than
        // letting Save silently do nothing (#5077), and say why on the
        // accessible channel as well as the tooltip.
        const auto updateSaveState = [saveProfileBtn](const QString& text) {
            const bool named = !text.trimmed().isEmpty();
            saveProfileBtn->setEnabled(named);
            const QString reason = named
                ? QStringLiteral("Save the current bindings as a named profile")
                : QStringLiteral("Type a profile name to save the current bindings");
            saveProfileBtn->setToolTip(reason);
            saveProfileBtn->setAccessibleDescription(reason);
        };
        updateSaveState(m_profileCombo->currentText());
        connect(m_profileCombo, &QComboBox::currentTextChanged, saveProfileBtn, updateSaveState);
        connect(saveProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;  // unreachable while disabled; kept as the guard
            if (!MidiSettings::isValidProfileName(name)) {
                // The store refuses these names silently; say why here so the
                // operator isn't left with a Save that does nothing. (#4975)
                showNotice(QStringLiteral("Save Profile"),
                    QStringLiteral("\"%1\" isn't a valid profile name — a name "
                                   "can't contain / or \\ or start with \".\".")
                        .arg(name));
                return;
            }
            // Sampled before the write so the result can say which success
            // happened — an overwrite is otherwise invisible, because the list
            // refresh changes nothing. Asked of the filesystem, so it matches
            // what the write does on case-insensitive volumes too. (#5077)
            const int count = m_manager->bindings().size();
            if (count == 0) {
                // The store refuses an empty set (it would replace the profile
                // with nothing); say so here so the refusal isn't reported as
                // an unwritable directory.
                showNotice(QStringLiteral("Save Profile"),
                    QStringLiteral("No bindings to save — add a binding first."));
                return;
            }
            const bool existed = MidiSettings::profileExists(name);
            if (!MidiSettings::instance().saveProfile(name, m_manager->bindings())) {
                showNotice(QStringLiteral("Save Profile"),
                    QStringLiteral("Couldn't write profile \"%1\" — check that %2 "
                                   "is writable.")
                        .arg(name, QDir::toNativeSeparators(MidiSettings::profileDir())));
                return;
            }
            refreshProfileList();
            showNotice(QStringLiteral("Save Profile"),
                QStringLiteral("%1 profile \"%2\" (%3 binding%4).")
                    .arg(existed ? QStringLiteral("Overwrote") : QStringLiteral("Saved"),
                         name, QString::number(count),
                         count == 1 ? QString() : QStringLiteral("s")));
        });
        btnRow->addWidget(saveProfileBtn);

        auto* loadProfileBtn = new QPushButton("Load");
        loadProfileBtn->setObjectName(QStringLiteral("midiProfileLoadButton"));
        loadProfileBtn->setAccessibleName(QStringLiteral("Load MIDI profile"));
        loadProfileBtn->setToolTip(
            QStringLiteral("Apply the selected profile to the current bindings"));
        connect(loadProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;
            auto bindings = MidiSettings::instance().loadProfile(name);
            if (bindings.isEmpty()) {
                // A missing or empty profile silently doing nothing is the
                // same failure class the importer refuses — say so instead.
                showNotice(QStringLiteral("Load Profile"),
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

        auto* importProfileBtn = new QPushButton("Import...");
        importProfileBtn->setObjectName(QStringLiteral("midiProfileImportButton"));
        importProfileBtn->setAccessibleName(QStringLiteral("Import MIDI profile"));
        importProfileBtn->setToolTip(QStringLiteral(
            "Import a profile file into the store — AetherSDR profile XML or SmartSDR \".map\""));
        connect(importProfileBtn, &QPushButton::clicked, this,
                &MidiMappingDialog::importProfileFromFile);
        btnRow->addWidget(importProfileBtn);

        auto* exportProfileBtn = new QPushButton("Export...");
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

    // 40 px taller than before for the canon header, and never smaller than
    // the layout needs at this theme's font.
    setMinimumSize(QSize(700, 590).expandedTo(minimumSizeHint()));

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
    const QPointer<MidiMappingDialog> self(this);
    const QPointer<MidiControlManager> manager(m_manager);
    ScopedChildWidget<QFileDialog> dialogOwner(this);
    QFileDialog& dialog = *dialogOwner.get();
    dialog.setWindowTitle(QStringLiteral("Import MIDI Profile"));
    dialog.setDirectory(midiTransferDirectory());
    dialog.setNameFilter(QStringLiteral("MIDI profiles (*.xml *.map);;All files (*)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    const int dialogResult = dialog.exec();
    if (!self || !dialogOwner || !manager || dialogResult != QDialog::Accepted
        || dialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    rememberMidiTransferDirectory(path);

    const MidiImportResult result = MidiSettings::instance().importProfile(
        path,
        [manager](const QString& id) { return manager && manager->findParam(id) != nullptr; });

    const QString fileName = QFileInfo(path).fileName();
    if (!result.ok()) {
        showNotice(QStringLiteral("Import MIDI Profile"),
                   QStringLiteral("No bindings were imported from %1.").arg(fileName),
                   QString(), result.errors.join(QLatin1Char('\n')));
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
        showNotice(QStringLiteral("Import MIDI Profile"),
                   QStringLiteral("No usable bindings in %1.").arg(fileName),
                   informativeLines.join(QLatin1Char('\n')),
                   detailLines.join(QLatin1Char('\n')));
        return;
    }

    refreshProfileList();
    m_profileCombo->setCurrentText(result.profileName);

    showNotice(QStringLiteral("Import MIDI Profile"),
               QStringLiteral("Imported %1 binding(s) from %2 as profile \"%3\". "
                              "Click Load to apply it.")
                   .arg(result.importedCount)
                   .arg(fileName, result.profileName),
               informativeLines.join(QLatin1Char('\n')),
               detailLines.join(QLatin1Char('\n')));
}

void MidiMappingDialog::exportProfileToFile()
{
    const QPointer<MidiMappingDialog> self(this);
    const QPointer<MidiControlManager> manager(m_manager);
    if (!manager) {
        return;
    }
    const auto bindings = manager->bindings();
    if (bindings.isEmpty()) {
        showNotice(QStringLiteral("Export MIDI Profile"),
                                         QStringLiteral("There are no bindings to export."));
        return;
    }

    // yyyyMMdd_HHmmss so two exports the same day don't suggest the identical
    // filename (matches the shortcut exporter).
    const QString fileName = QStringLiteral("AetherSDR_MidiProfile_%1_v%2.xml")
                                 .arg(QDateTime::currentDateTime().toString(
                                          QStringLiteral("yyyyMMdd_HHmmss")),
                                      QCoreApplication::applicationVersion());
    ScopedChildWidget<QFileDialog> dialogOwner(this);
    QFileDialog& dialog = *dialogOwner.get();
    dialog.setWindowTitle(QStringLiteral("Export MIDI Profile"));
    const QString initialPath = QDir(midiTransferDirectory()).filePath(fileName);
    dialog.setDirectory(QFileInfo(initialPath).absolutePath());
    dialog.selectFile(QFileInfo(initialPath).fileName());
    dialog.setNameFilter(QStringLiteral("MIDI profile XML (*.xml)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("xml"));
    const int dialogResult = dialog.exec();
    if (!self || !dialogOwner || !manager || dialogResult != QDialog::Accepted
        || dialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    rememberMidiTransferDirectory(path);

    const MidiExportResult result = MidiSettings::instance().exportProfile(path, bindings);
    if (!result.ok()) {
        showNotice(QStringLiteral("Export MIDI Profile"),
                                     result.error);
        return;
    }
    showNotice(QStringLiteral("Export MIDI Profile"),
        QStringLiteral("Exported %1 binding(s) to %2.")
            .arg(result.exportedCount)
            .arg(QFileInfo(path).fileName()));
}

void MidiMappingDialog::showNotice(const QString& title, const QString& text,
                                   const QString& informative, const QString& details)
{
    ScopedChildWidget<CanonMessageBox> boxOwner(title, text, this);
    CanonMessageBox& box = *boxOwner.get();
    if (!informative.isEmpty())
        box.setInformativeText(informative);
    if (!details.isEmpty())
        box.setDetailedText(details);
    box.setDefaultButton(box.addButton(QStringLiteral("OK"), CanonMessageBox::Role::Accept));
    box.exec();
}

void MidiMappingDialog::setPortStatus(const QString& text, const QString& colorToken)
{
    if (!m_statusLabel)
        return;
    m_statusLabel->setText(text);
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        m_statusLabel,
        QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(colorToken));
}

void MidiMappingDialog::refreshPortList()
{
    if (!m_portCombo)
        return;
    // Never rebuild the list while its dropdown is open: the row under the
    // operator's cursor would change identity mid-gesture.
    if (m_portCombo->view() && m_portCombo->view()->isVisible())
        return;

    // Repopulating emits currentIndexChanged from clear(); block it so a
    // refresh can never look like a selection the operator did not make.
    const QSignalBlocker blocker(m_portCombo);

    // Preserve the choice by NAME, not by row.  Precedence: what is selected
    // now, else the port actually open, else the remembered device.
    QString keep = m_portCombo->currentData().toString();
    if (keep.isEmpty()) {
        keep = m_manager->isOpen() ? m_manager->currentPortName()
                                   : MidiSettings::instance().lastDevice();
    }

    m_portCombo->clear();
    // The name is the only identity RtMidi exposes; it goes in item data so
    // every consumer resolves by it instead of by index.  See
    // connectToSelectedPort() for why the index is not usable.
    for (const auto& port : m_manager->availablePorts())
        m_portCombo->addItem(port, port);

    int selected = keep.isEmpty() ? -1 : m_portCombo->findData(keep);
    if (selected < 0 && !keep.isEmpty()) {
        // A configured (or still-open) port the platform no longer lists.
        // Keep it visible and labelled rather than silently sliding the
        // operator's choice onto whatever now occupies row 0.
        m_portCombo->insertItem(0, keep + " (not connected)", keep);
        selected = 0;
    }
    if (m_portCombo->count() == 0) {
        m_portCombo->addItem("No MIDI inputs found", QString());
        selected = 0;
    }
    m_portCombo->setCurrentIndex(selected < 0 ? 0 : selected);
}

bool MidiMappingDialog::connectToSelectedPort()
{
    if (!m_portCombo)
        return false;

    // Resolve the STABLE IDENTITY held in item data, never m_portCombo's row.
    // MidiControlManager::openPort(idx) indexes a FRESH RtMidi enumeration
    // taken inside the call, while the combo holds whatever was enumerated at
    // the last build or Refresh.  A device that arrived or vanished in between
    // shifts every row below it, so passing the row through opens different
    // hardware than the entry named on screen.
    const QString wanted = m_portCombo->currentData().toString();
    if (wanted.isEmpty())
        return false;

    const QStringList ports = m_manager->availablePorts();
    const int idx = ports.indexOf(wanted);   // exact match, not a substring
    if (idx < 0) {
        setPortStatus(QStringLiteral("\"%1\" is no longer connected").arg(wanted),
                      QStringLiteral("{{color.accent.warning}}"));
        refreshPortList();
        return false;
    }

    if (!m_manager->openPort(idx))
        return false;   // openPort() already emitted portError()

    // Catch a changed index-to-name mapping at the manager's name lookup.
    // currentPortName() is cached before RtMidi opens the native endpoint;
    // this narrows the race but cannot detect a later topology change inside
    // the native open call. Duplicate names also remain indistinguishable.
    if (m_manager->currentPortName() != wanted) {
        m_manager->closePort();
        setPortStatus(
            QStringLiteral("Port list changed while connecting — press Refresh"),
            QStringLiteral("{{color.accent.warning}}"));
        refreshPortList();
        return false;
    }

    m_connectBtn->setText("Disconnect");
    setPortStatus("Connected: " + m_manager->currentPortName(),
                  QStringLiteral("{{color.accent.success}}"));
    // Save device preference
    auto& ms = MidiSettings::instance();
    ms.setLastDevice(m_manager->currentPortName());
    ms.save();
    return true;
}

void MidiMappingDialog::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    refreshPortList();
    // The Connect button and status line are also built once and can be
    // describing a connection state that changed while this window was
    // hidden: MidiControlManager's 5 s hotplug timer re-opens a remembered
    // port on its own (openPortByName) and nothing here listens for
    // portOpened/portClosed. Re-read the manager rather than trusting them.
    const bool open = m_manager->isOpen();
    m_connectBtn->setText(open ? "Disconnect" : "Connect");
    setPortStatus(open ? "Connected: " + m_manager->currentPortName()
                       : QStringLiteral("Disconnected"),
                  open ? QStringLiteral("{{color.accent.success}}")
                       : QStringLiteral("{{color.text.label}}"));

    // Every edit here saves to midi.settings, and those saves are refused
    // after a failed startup read. Say so once per open, after the window is
    // up, rather than letting each edit fail silently. (#6683)
    if (!m_savesBlockedNoticeShown && MidiSettings::instance().savesBlocked()) {
        m_savesBlockedNoticeShown = true;
        QTimer::singleShot(0, this, [this] {
            showNotice(QStringLiteral("MIDI Changes Not Saved"),
                QStringLiteral("AetherSDR couldn't read midi.settings at startup, so "
                               "MIDI binding and device preference changes won't be "
                               "saved this session. The file was left unchanged. "
                               "Restore read access or a valid backup, then restart "
                               "AetherSDR."));
        });
    }
}

void MidiMappingDialog::hideEvent(QHideEvent* event)
{
    // A spontaneous hide is the OS minimising the window (Win+D, the macOS
    // Dock): the operator may be reaching for the controller, so Learn stays
    // armed. Closing sends a real hide, minimised or not.
    if (!event->spontaneous() && m_manager && m_manager->isLearning())
        m_manager->cancelLearn();
    CanonWindow::hideEvent(event);
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
        invertCheck->setAccessibleName(QString("Invert %1").arg(paramName));
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
        relCheck->setAccessibleName(QString("Relative %1").arg(paramName));
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
        AetherSDR::ThemeManager::instance().applyStyleSheet(delBtn, kBtnGlyphDestructiveStyle);
        delBtn->setToolTip("Remove this binding");
        delBtn->setAccessibleName(QString("Remove binding for %1").arg(paramName));
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
    const QString stableParamId(paramId);
    const QPointer<MidiMappingDialog> self(this);
    const QPointer<MidiControlManager> manager(m_manager);
    if (!manager) {
        return;
    }
    // A stray controller touch must not complete a half-armed Learn while the
    // operator is typing in this form.
    if (manager->isLearning()) {
        manager->cancelLearn();
    }

    const MidiParam* param = manager->findParam(stableParamId);
    const QString paramLabel = param
        ? QString("[%1] %2").arg(param->category, param->displayName) : stableParamId;
    // Learn forces relative=true on VFO CC captures (onMidiMessage); the form
    // mirrors that as a default so a typed VFO knob behaves like a learned one.
    const bool isVfoKnob = MidiControlManager::isVfoTuneKnob(stableParamId);

    // A canon dialog in the window's scope, as Net Scheduler's editor is.
    // Return on a field clicks OK, the default; Esc, the ✕ and Cancel add nothing.
    const QString editorTitle = existing ? QStringLiteral("Edit MIDI Binding")
                                         : QStringLiteral("Add MIDI Binding");
    ScopedChildWidget<CanonWindow> dialogOwner(editorTitle, this);
    CanonWindow& dlg = *dialogOwner.get();
    dlg.setModal(true);
    dlg.setObjectName("midiManualBindingDialog");
    theme::setContainer(&dlg, QStringLiteral("canon/midiMapping"));
    applyCanonSheet(dlg.bodyWidget(), [] {
        // OK is outlined in cyan as the default, as CanonMessageBox draws it.
        return midiMappingSheet()
             + QStringLiteral("QPushButton:default { color: {{color.canon.cyan}};"
                              " border-color: {{color.canon.cyan}}; }");
    });

    auto* editorRoot = new QVBoxLayout(dlg.bodyWidget());
    editorRoot->setContentsMargins(14, 12, 14, 14);
    editorRoot->setSpacing(10);
    editorRoot->addWidget(makeCanonHeader(editorTitle));
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(8);
    editorRoot->addLayout(form);

    auto* bindingFor = new QLabel(QString("Binding for: %1").arg(paramLabel));
    bindingFor->setTextFormat(Qt::PlainText);
    form->addRow(bindingFor);

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

    // Cancel, then OK at the far right, as CanonMessageBox lays out its row.
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dlg);
    cancelBtn->setObjectName(QStringLiteral("manualCancelButton"));
    cancelBtn->setAutoDefault(false);
    auto* okBtn = new QPushButton(QStringLiteral("OK"), &dlg);
    okBtn->setObjectName(QStringLiteral("manualOkButton"));
    okBtn->setDefault(true);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    buttonRow->addWidget(cancelBtn);
    buttonRow->addWidget(okBtn);
    editorRoot->addLayout(buttonRow);

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
    // The Dialog-scale field padding (12 px) plus the 1 px border.
    ModemChrome::insetSpinText(&dlg, 13);

    const int dialogResult = dlg.exec();
    if (!self || !dialogOwner || !manager || dialogResult != QDialog::Accepted) {
        return;
    }

    MidiBinding b;
    b.channel  = channelCombo->currentData().toInt();
    b.msgType  = MidiBinding::MsgType(typeCombo->currentData().toInt());
    // Learn stores -1 for Pitch Bend (the message carries no number); mirror it.
    b.number   = (b.msgType == MidiBinding::PitchBend) ? -1 : numberSpin->value();
    b.paramId  = stableParamId;
    b.inverted = invertCheck->isChecked();
    b.relative = relativeCheck->isChecked() && b.msgType == MidiBinding::CC;

    // Duplicate-source guard. The dispatch index is keyed on (channel, type,
    // number) without the param, so on a collision the last table row silently
    // wins and the older binding stops responding with no indication. Learn
    // rarely produces this; a typed form is one typo away. Never shadow
    // silently — name the loser and ask.
    QStringList shadowedNames;
    QStringList shadowedIds;
    for (const auto& cur : manager->bindings()) {
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
            const MidiParam* cp = manager->findParam(cur.paramId);
            shadowedNames << (cp ? QString("[%1] %2").arg(cp->category, cp->displayName)
                                 : cur.paramId);
            shadowedIds << cur.paramId;
        }
    }
    if (!shadowedIds.isEmpty()) {
        // Replacing removes bindings: Cancel is the default, and Cancel,
        // Return, Esc or the ✕ replace nothing.
        const QString plural = shadowedIds.size() > 1 ? QStringLiteral("s") : QString();
        ScopedChildWidget<CanonMessageBox> boxOwner(
            QStringLiteral("Source Already Bound"),
            QString("%1 is already bound to:").arg(b.sourceDisplayName()), this);
        CanonMessageBox& box = *boxOwner.get();
        box.setDetailedText(shadowedNames.join(QLatin1Char('\n')));
        box.setInformativeText(
            QString("Two bindings on the same or overlapping source cannot both "
                    "respond on the channel they share — the older one would "
                    "silently stop working there.\n\nReplace the existing binding%1?")
                .arg(plural));
        auto* cancel = box.addButton(QStringLiteral("Cancel"), CanonMessageBox::Role::Reject);
        auto* replace = box.addButton(QStringLiteral("Replace"), CanonMessageBox::Role::Destructive);
        box.setDefaultButton(cancel);
        box.exec();
        if (!self || !manager || !boxOwner) {
            return;
        }
        if (box.clickedButton() != replace)
            return;
        for (const auto& id : shadowedIds)
            manager->removeBinding(id);
    }

    manager->addBinding(b);
    refreshBindingTable();
    MidiSettings::instance().saveBindings(manager->bindings());
}

} // namespace AetherSDR

#endif // HAVE_MIDI
