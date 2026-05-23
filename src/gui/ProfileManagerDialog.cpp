#include "ProfileManagerDialog.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>

namespace MasterSDR {

static const QString kDialogStyle =
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QTabWidget::pane { border: 1px solid #203040; background: #0f0f1a; }"
    "QTabBar::tab { background: #1a2a3a; color: #8898a8; padding: 6px 14px;"
    "  border: 1px solid #203040; border-bottom: none; border-top-left-radius: 4px;"
    "  border-top-right-radius: 4px; margin-right: 2px; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8; }"
    "QLineEdit { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 4px 6px; color: #c8d8e8; }"
    "QListWidget { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; }"
    "QListWidget::item:selected { background: #0070c0; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QCheckBox { color: #c8d8e8; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }";

ProfileManagerDialog::ProfileManagerDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog("Profile Manager", "ProfileManagerDialogGeometry", parent),
      m_model(model)
{
    setMinimumSize(460, 400);
    setStyleSheet(kDialogStyle);

    // PersistentDialog::setFramelessMode() owns the body layout's contents
    // margins (9 / 9-or-7 / 9 / 9 depending on frameless chrome state).
    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(9);

    m_tabs = new QTabWidget;

    // Global tab
    m_tabs->addTab(
        buildProfileTab("global", model->globalProfiles(),
                        model->activeGlobalProfile()),
        "Global");

    // Transmit tab
    m_tabs->addTab(
        buildProfileTab("transmit", model->transmitModel().profileList(),
                        model->transmitModel().activeProfile()),
        "Transmit");

    // Microphone tab
    m_tabs->addTab(
        buildProfileTab("mic", model->transmitModel().micProfileList(),
                        model->transmitModel().activeMicProfile()),
        "Microphone");

    // Auto-Save tab
    m_tabs->addTab(buildAutoSaveTab(), "Auto-Save");

    root->addWidget(m_tabs);

    // Close button
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // Listen for profile list updates
    connect(model, &RadioModel::globalProfilesChanged, this, [this] {
        refreshTab("global");
    });
    connect(&model->transmitModel(), &TransmitModel::profileListChanged, this, [this] {
        refreshTab("transmit");
    });
    connect(&model->transmitModel(), &TransmitModel::micProfileListChanged, this, [this] {
        refreshTab("mic");
    });
}

QWidget* ProfileManagerDialog::buildProfileTab(const QString& type,
                                                const QStringList& profiles,
                                                const QString& active)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    // New profile name entry
    auto* nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("New Profile Name");
    vbox->addWidget(nameEdit);

    // Buttons: Load, Save/Create, Delete.
    // FlexLib (Radio.cs:8394, 8435) marks `profile transmit/mic save` obsolete
    // with `error: true` — only the global profile supports an explicit overwrite
    // command. TX/Mic profiles update via autosave instead, so the button on
    // those tabs is labelled "Create" to reflect what the radio actually does.
    auto* btnRow = new QHBoxLayout;
    auto* loadBtn = new QPushButton("Load");
    const bool isGlobal = (type == "global");
    auto* saveBtn = new QPushButton(isGlobal ? "Save" : "Create");
    auto* deleteBtn = new QPushButton("Delete");

    loadBtn->setEnabled(false);
    deleteBtn->setEnabled(false);

    btnRow->addWidget(loadBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addWidget(deleteBtn);
    vbox->addLayout(btnRow);

    if (!isGlobal) {
        auto* note = new QLabel(
            "Updates to existing profiles save automatically — enable\n"
            "Auto-Save (Auto-Save tab) so changes follow the active profile.\n"
            "Create makes a new profile; it does not overwrite an existing one.");
        note->setStyleSheet("QLabel { color: #6888a0; font-size: 11px; }");
        note->setWordWrap(true);
        vbox->addWidget(note);
    }

    // Profile list
    auto* list = new QListWidget;
    for (const auto& p : profiles) {
        auto* item = new QListWidgetItem(p);
        if (p == active)
            item->setSelected(true);
        list->addItem(item);
    }
    vbox->addWidget(list, 1);

    // Store refs
    m_tabWidgets[type] = {nameEdit, list, loadBtn, saveBtn, deleteBtn};

    // Selection enables Load/Delete and populates the name field
    connect(list, &QListWidget::currentItemChanged, this,
            [nameEdit, loadBtn, deleteBtn](QListWidgetItem* current, QListWidgetItem*) {
        loadBtn->setEnabled(current != nullptr);
        deleteBtn->setEnabled(current != nullptr);
        if (current)
            nameEdit->setText(current->text());
    });

    // Double-click loads
    connect(list, &QListWidget::itemDoubleClicked, this,
            [this, type](QListWidgetItem* item) {
        if (!item) return;
        const QString name = item->text();
        if (type == "global")
            m_model->loadGlobalProfile(name);
        else if (type == "transmit")
            m_model->sendCommand(QString("profile transmit load \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic load \"%1\"").arg(name));
    });

    // Load button
    connect(loadBtn, &QPushButton::clicked, this, [this, type, list] {
        auto* item = list->currentItem();
        if (!item) return;
        const QString name = item->text();
        if (type == "global")
            m_model->loadGlobalProfile(name);
        else if (type == "transmit")
            m_model->sendCommand(QString("profile transmit load \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic load \"%1\"").arg(name));
    });

    // Save/Create button — Global truly saves (creates or overwrites); TX/Mic
    // can only create (FlexLib Radio.cs:8394, 8435). For TX/Mic, refuse to
    // silently no-op against an existing name: explain the autosave model so
    // the user knows their updates aren't being captured.
    connect(saveBtn, &QPushButton::clicked, this, [this, type, nameEdit, list] {
        QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) {
            auto* item = list->currentItem();
            if (item) name = item->text();
        }
        if (name.isEmpty()) return;

        if (type == "global") {
            m_model->sendCommand(QString("profile global save \"%1\"").arg(name));
        } else {
            bool exists = false;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->text() == name) { exists = true; break; }
            }
            if (exists) {
                const QString kind = (type == "transmit") ? "TX" : "Mic";
                if (!m_model->autoSave()) {
                    // Offer Auto-Save inline so the user can act on the
                    // remedy without hunting for the Auto-Save tab.
                    QMessageBox box(this);
                    box.setWindowTitle("Profile already exists");
                    box.setIcon(QMessageBox::Question);
                    box.setText(
                        QString("A %1 profile named \"%2\" already exists.").arg(kind, name));
                    box.setInformativeText(
                        QString("The radio can't overwrite %1 profiles directly — updates "
                                "are captured by Auto-Save while the profile is active. "
                                "Auto-Save is currently OFF.\n\n"
                                "Would you like to enable Auto-Save now so your changes "
                                "to \"%2\" are captured?").arg(kind, name));
                    auto* enableBtn = box.addButton("Enable Auto-Save", QMessageBox::AcceptRole);
                    box.addButton("Close", QMessageBox::RejectRole);
                    box.setDefaultButton(enableBtn);
                    box.exec();
                    if (box.clickedButton() == enableBtn) {
                        // The sibling Auto-Save tab checkbox is wired to
                        // RadioModel::autoSaveChanged below, so the radio's
                        // confirmation of auto_save=1 will sync it.
                        m_model->sendCommand("profile autosave on");
                    }
                    return;
                }
                QMessageBox::information(this, "Profile already exists",
                    QString("A %1 profile named \"%2\" already exists.\n\n"
                            "The radio cannot overwrite %1 profiles directly. "
                            "Updates are captured by Auto-Save (currently ON) "
                            "while the profile is active.\n\n"
                            "To replace this profile, delete it first and then "
                            "Create it again.")
                        .arg(kind, name));
                return;
            }
            if (type == "transmit")
                m_model->sendCommand(QString("profile transmit create \"%1\"").arg(name));
            else
                m_model->sendCommand(QString("profile mic create \"%1\"").arg(name));
        }

        nameEdit->clear();
        // Radio will push updated list via status
    });

    // Delete button
    connect(deleteBtn, &QPushButton::clicked, this, [this, type, list] {
        auto* item = list->currentItem();
        if (!item) return;
        const QString name = item->text();

        auto reply = QMessageBox::question(this, "Delete Profile",
            QString("Delete profile \"%1\"?").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;

        if (type == "global")
            m_model->sendCommand(QString("profile global delete \"%1\"").arg(name));
        else if (type == "transmit")
            m_model->sendCommand(QString("profile transmit delete \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic delete \"%1\"").arg(name));
    });

    return page;
}

QWidget* ProfileManagerDialog::buildAutoSaveTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto* desc = new QLabel(
        "When auto-save is enabled, changes to TX and Mic\n"
        "settings are automatically saved to the active profile.");
    desc->setStyleSheet("QLabel { color: #6888a0; font-size: 11px; }");
    desc->setWordWrap(true);
    vbox->addWidget(desc);
    vbox->addSpacing(10);

    m_autoSaveTx = new QCheckBox("Auto-save profile changes");

    // Read initial state from radio (auto_save in radio status)
    m_autoSaveTx->setChecked(m_model->autoSave());

    connect(m_autoSaveTx, &QCheckBox::toggled, this, [this](bool on) {
        m_model->sendCommand(QString("profile autosave %1").arg(on ? "on" : "off"));
    });

    // React to Auto-Save flips that originate outside this checkbox —
    // the Profile Manager "Enable Auto-Save" affirmation, TCI clients,
    // profile load, or remote SmartSDR clients all flip auto_save via
    // the radio.  QSignalBlocker prevents the resulting setChecked from
    // bouncing back into ::toggled and re-issuing the radio command.
    connect(m_model, &RadioModel::autoSaveChanged, this, [this](bool on) {
        if (!m_autoSaveTx) return;
        QSignalBlocker block(m_autoSaveTx);
        m_autoSaveTx->setChecked(on);
    });

    vbox->addWidget(m_autoSaveTx);
    vbox->addStretch();

    return page;
}

void ProfileManagerDialog::refreshTab(const QString& type)
{
    if (!m_tabWidgets.contains(type)) return;
    auto& tw = m_tabWidgets[type];

    QStringList profiles;
    QString active;

    if (type == "global") {
        profiles = m_model->globalProfiles();
        active = m_model->activeGlobalProfile();
    } else if (type == "transmit") {
        profiles = m_model->transmitModel().profileList();
        active = m_model->transmitModel().activeProfile();
    } else if (type == "mic") {
        profiles = m_model->transmitModel().micProfileList();
        active = m_model->transmitModel().activeMicProfile();
    }

    tw.list->clear();
    for (const auto& p : profiles) {
        auto* item = new QListWidgetItem(p);
        tw.list->addItem(item);
        if (p == active)
            tw.list->setCurrentItem(item);
    }
}

} // namespace MasterSDR
