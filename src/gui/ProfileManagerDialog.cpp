#include "ProfileManagerDialog.h"
#include "core/ThemeManager.h"
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
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>

namespace AetherSDR {

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
    // Without this the dialog's explicit QPushButton color/background defeats
    // Qt's default disabled rendering, so a disabled button is pixel-identical
    // to an enabled one — the "no target" state (#4396) would be invisible.
    "QPushButton:disabled { background: #141c26; border: 1px solid #1a2632;"
    "  color: #5a6a78; }"
    "QCheckBox { color: #c8d8e8; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }";

// Result-line styling, matching ConnectionPanel's manual-connect result label
// (ConnectionPanel.cpp:38-41) so the two read as the same kind of message.
static const char* kResultInfoStyle =
    "QLabel { color: #9bd1ff; font-size: 11px; background: transparent; border: none; }";
static const char* kResultErrorStyle =
    "QLabel { color: #ff8f8f; font-size: 11px; background: transparent; border: none; }";

// How long a Global save may sit unanswered before the result line stops
// claiming it is still in flight. A profile save is a radio-side flash write,
// so it is slower than a routine command; this is generous enough not to fire
// on a merely slow radio, short enough that a dead session does not leave the
// dialog lying about its state indefinitely.
static constexpr int kSaveResponseTimeoutMs = 15000;

ProfileManagerDialog::ProfileManagerDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog("Profile Manager", "ProfileManagerDialogGeometry", parent),
      m_model(model)
{
    theme::setContainer(this, QStringLiteral("dialog/profileManager"));
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

    // New profile name entry.  The three tabs build identical widget classes,
    // so object names are what let an assertion (or the automation bridge) name
    // *which* tab's field it means.
    auto* nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("New Profile Name");
    nameEdit->setObjectName(QString("profileNameEdit_%1").arg(type));
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
    saveBtn->setObjectName(QString("profileSaveBtn_%1").arg(type));

    loadBtn->setEnabled(false);
    deleteBtn->setEnabled(false);
    // An empty name field means "no target" (#4396).  Disabling up front says so
    // before the click, instead of letting Save look armed and then do nothing.
    saveBtn->setEnabled(!nameEdit->text().trimmed().isEmpty());

    btnRow->addWidget(loadBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addWidget(deleteBtn);
    vbox->addLayout(btnRow);

    // Result line for the last Save/Create on this tab (#4362).  Hidden until
    // there is something to report, so it adds no height to the resting dialog.
    auto* status = new QLabel;
    status->setObjectName(QString("profileStatus_%1").arg(type));
    status->setWordWrap(true);
    status->setVisible(false);
    vbox->addWidget(status);

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
    m_tabWidgets[type] = {nameEdit, list, loadBtn, saveBtn, deleteBtn, status};

    // Selection enables Load/Delete and populates the name field
    connect(list, &QListWidget::currentItemChanged, this,
            [nameEdit, loadBtn, deleteBtn](QListWidgetItem* current, QListWidgetItem*) {
        loadBtn->setEnabled(current != nullptr);
        deleteBtn->setEnabled(current != nullptr);
        if (current)
            nameEdit->setText(current->text());
    });

    // Two signals, deliberately: textChanged also fires for the programmatic
    // fills (#177 select->auto-fill, refreshTab's setCurrentItem, the post-save
    // clear), which is exactly what the enable gate must track.  textEdited
    // fires only for a human keystroke — so retyping a name drops a stale
    // result line, while the radio's own status push cannot wipe a result the
    // user has not seen yet.
    connect(nameEdit, &QLineEdit::textChanged, saveBtn, [saveBtn](const QString& t) {
        saveBtn->setEnabled(!t.trimmed().isEmpty());
    });
    connect(nameEdit, &QLineEdit::textEdited, this, [this, type] {
        setTabStatus(type, QString(), false);
    });

    // Double-click loads
    connect(list, &QListWidget::itemDoubleClicked, this,
            [this, type](QListWidgetItem* item) {
        if (!item) return;
        const QString name = item->text();
        setTabStatus(type, QString(), false);   // same staleness as Load (#4362)
        if (type == "global")
            m_model->loadGlobalProfile(name);
        else if (type == "transmit")
            m_model->sendCommand(QString("profile tx load \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic load \"%1\"").arg(name));
    });

    // Load button
    connect(loadBtn, &QPushButton::clicked, this, [this, type, list] {
        auto* item = list->currentItem();
        if (!item) return;
        const QString name = item->text();
        // The result line describes the last Save on this tab; a Load makes it
        // stale (and after a Delete it would name a profile that no longer
        // exists). Only a human keystroke clears it otherwise (#4362).
        setTabStatus(type, QString(), false);
        if (type == "global")
            m_model->loadGlobalProfile(name);
        else if (type == "transmit")
            m_model->sendCommand(QString("profile tx load \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic load \"%1\"").arg(name));
    });

    // Save/Create button — Global truly saves (creates or overwrites); TX/Mic
    // can only create (FlexLib Radio.cs:8394, 8435). For TX/Mic, refuse to
    // silently no-op against an existing name: explain the autosave model so
    // the user knows their updates aren't being captured.
    connect(saveBtn, &QPushButton::clicked, this, [this, type, nameEdit, list] {
        // No fallback to the highlighted row (#4396).  That fallback predates
        // #177's select->auto-fill, when it was the only way to re-save an
        // existing profile; now selection always fills the field, so it could
        // only ever fire after the user deliberately cleared it — overwriting
        // whichever row happened to be highlighted, silently.  A blank field
        // means no target.  (The button is disabled in that state; this is the
        // backstop.)
        const QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) return;

        if (type == "global") {
            // A dropped command never produces an R-line, so "Saving..." would
            // otherwise sit there forever claiming a save is in progress. There
            // is no menu guard on opening this dialog, so the disconnected case
            // is reachable directly: sendCmd() still allocates a seq and stores
            // the callback, but RadioConnection::writeCommand() early-returns on
            // !isConnected() and nothing ever resolves it (WanConnection::
            // sendCommand() likewise returns 0 without invoking the callback).
            // Say so up front instead of sending into the void.
            if (!m_model || !m_model->isConnected()) {
                setTabStatus(
                    type,
                    QString("Not connected — cannot save \"%1\".").arg(name),
                    true);
                return;
            }

            // The R-line result code is the only thing that distinguishes a
            // completed overwrite from a refused one (#4362) — the list refresh
            // is a visual no-op when the name already exists.  QPointer-guarded
            // because a pending callback is only erased by its matching
            // response: if the dialog goes away first, the reply must find a
            // null guard rather than freed widgets.
            const QPointer<ProfileManagerDialog> self(this);
            const quint64 token = ++m_saveSeq;
            m_pendingSaveToken[type] = token;
            setTabStatus(type, QString("Saving \"%1\"...").arg(name), false);
            m_model->sendCmdPublic(
                QString("profile global save \"%1\"").arg(name),
                [self, type, name, token](int code, const QString& body) {
                    if (!self) return;
                    // Superseded by a newer save on this tab, or already
                    // resolved by the timeout — either way, stay quiet.
                    if (self->m_pendingSaveToken.value(type) != token) return;
                    self->m_pendingSaveToken[type] = 0;
                    if (code == 0) {
                        // Clear only if the field still holds what we saved: a
                        // reply that lands after the user started typing the
                        // next name must not erase that half-typed entry. The
                        // clear is programmatic, so it moves the enable gate
                        // without touching the result line.
                        if (self->m_tabWidgets.contains(type)) {
                            QLineEdit* edit = self->m_tabWidgets[type].nameEdit;
                            if (edit && edit->text().trimmed() == name)
                                edit->clear();
                        }
                        self->setTabStatus(
                            type, QString("Saved profile \"%1\".").arg(name), false);
                    } else {
                        // Keep the name field: the user's target survives for a
                        // retry instead of being cleared as if it had worked.
                        const QString detail = body.trimmed();
                        self->setTabStatus(
                            type,
                            detail.isEmpty()
                                ? QString("Radio refused save of \"%1\" (code 0x%2).")
                                      .arg(name).arg(code, 0, 16)
                                : QString("Radio refused save of \"%1\" (code 0x%2): %3")
                                      .arg(name).arg(code, 0, 16).arg(detail),
                            true);
                    }
                });

            // Backstop for a command that reaches the wire but is never
            // answered — the radio drops, or a SmartLink session ends, between
            // the send and the R-line. Without this the dialog keeps claiming
            // the save is in flight for the life of the window.
            QTimer::singleShot(kSaveResponseTimeoutMs, this,
                               [self, type, name, token] {
                if (!self) return;
                if (self->m_pendingSaveToken.value(type) != token) return;
                self->m_pendingSaveToken[type] = 0;
                self->setTabStatus(
                    type,
                    QString("No response from the radio for \"%1\" — the save "
                            "may not have been applied.").arg(name),
                    true);
            });
            return;
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

        // TX/Mic only — the Global path clears inside its success callback, so
        // a refused save keeps the user's target.
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

        // Drop the last Save result: leaving it up would keep reporting
        // 'Saved profile "X".' for the profile just deleted (#4362).
        setTabStatus(type, QString(), false);

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

void ProfileManagerDialog::setTabStatus(const QString& type, const QString& text,
                                        bool error)
{
    if (!m_tabWidgets.contains(type)) return;
    QLabel* status = m_tabWidgets[type].status;
    if (!status) return;

    if (text.isEmpty()) {
        status->clear();
        status->setVisible(false);
        return;
    }
    status->setText(text);
    status->setStyleSheet(error ? kResultErrorStyle : kResultInfoStyle);
    status->setVisible(true);
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

} // namespace AetherSDR
