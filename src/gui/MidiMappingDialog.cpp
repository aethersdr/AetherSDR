#ifdef HAVE_MIDI

#include "MidiMappingDialog.h"
#include "core/MidiControlManager.h"
#include "core/MidiSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QCheckBox>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>

namespace AetherSDR {

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

MidiMappingDialog::MidiMappingDialog(MidiControlManager* manager, QWidget* parent)
    : QDialog(parent), m_manager(manager)
{
    setWindowTitle("MIDI Controller Mapping");
    setMinimumSize(700, 550);
    setStyleSheet("QDialog { background: #0f0f1a; }");

    auto* root = new QVBoxLayout(this);
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

        auto* refreshBtn = new QPushButton("Refresh");
        refreshBtn->setStyleSheet(kBtnStyle);
        connect(refreshBtn, &QPushButton::clicked, this, &MidiMappingDialog::refreshPortList);
        grid->addWidget(refreshBtn, 0, 2);

        m_connectBtn = new QPushButton("Connect");
        m_connectBtn->setStyleSheet(kBtnStyle);
        connect(m_connectBtn, &QPushButton::clicked, this, [this] {
            if (m_manager->isOpen()) {
                m_manager->closePort();
                m_connectBtn->setText("Connect");
                m_statusLabel->setText("Disconnected");
                m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
            } else {
                int idx = m_portCombo->currentIndex();
                if (idx < 0) return;
                if (m_manager->openPort(idx)) {
                    m_connectBtn->setText("Disconnect");
                    m_statusLabel->setText("Connected: " + m_manager->currentPortName());
                    m_statusLabel->setStyleSheet("QLabel { color: #30d050; font-size: 11px; }");
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
        m_activityLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 10px; font-family: monospace; }");
        grid->addWidget(m_activityLabel, 1, 2, 1, 2);

        auto* autoConnect = new QCheckBox("Auto-connect on startup");
        autoConnect->setStyleSheet("QCheckBox { color: #c8d8e8; }");
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
        m_bindingTable->setColumnCount(6);
        m_bindingTable->setHorizontalHeaderLabels({"Parameter", "MIDI Source", "Channel", "Invert", "Relative", ""});
        m_bindingTable->horizontalHeader()->setStretchLastSection(false);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        m_bindingTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        m_bindingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_bindingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_bindingTable->verticalHeader()->setVisible(false);
        m_bindingTable->setStyleSheet(
            "QTableWidget { background: #0a0a14; color: #c8d8e8; "
            "border: 1px solid #203040; font-size: 11px; gridline-color: #203040; }"
            "QTableWidget::item:selected { background: #00b4d8; color: #0f0f1a; }"
            "QHeaderView::section { background: #1a1a2e; color: #8aa8c0; "
            "border: 1px solid #203040; padding: 3px; font-size: 11px; }");
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
        learnBtn->setStyleSheet(kBtnStyle);
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

        vbox->addLayout(addRow);

        // Button row
        auto* btnRow = new QHBoxLayout;
        auto* clearAllBtn = new QPushButton("Clear All");
        clearAllBtn->setStyleSheet(kBtnStyle);
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

        auto* saveProfileBtn = new QPushButton("Save");
        saveProfileBtn->setStyleSheet(kBtnStyle);
        connect(saveProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;
            MidiSettings::instance().saveProfile(name, m_manager->bindings());
            refreshProfileList();
        });
        btnRow->addWidget(saveProfileBtn);

        auto* loadProfileBtn = new QPushButton("Load");
        loadProfileBtn->setStyleSheet(kBtnStyle);
        connect(loadProfileBtn, &QPushButton::clicked, this, [this] {
            QString name = m_profileCombo->currentText().trimmed();
            if (name.isEmpty()) return;
            auto bindings = MidiSettings::instance().loadProfile(name);
            if (bindings.isEmpty()) return;
            m_manager->clearBindings();
            for (const auto& b : bindings)
                m_manager->addBinding(b);
            refreshBindingTable();
            MidiSettings::instance().saveBindings(m_manager->bindings());
        });
        btnRow->addWidget(loadProfileBtn);

        vbox->addLayout(btnRow);
        root->addWidget(group, 1);
    }

    // ── Close button ────────────────────────────────────────────────────
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton("Close");
    closeBtn->setStyleSheet(kBtnStyle);
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
        m_bindingTable->setCellWidget(i, 5, delBtn);
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

} // namespace AetherSDR

#endif // HAVE_MIDI
