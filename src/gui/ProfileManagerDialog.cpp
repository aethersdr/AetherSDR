#include "ProfileManagerDialog.h"
#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "ControlAvailabilityRegistry.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"
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
#include <QPointer>
#include <QStyle>
#include <QSignalBlocker>
#include <QTimer>
#include "ScopedChildWidget.h"

namespace AetherSDR {

namespace {
// The tab's label, for accessible names ("Transmit profile name").
QString tabLabel(const QString& type)
{
    return type == QLatin1String("global") ? QStringLiteral("Global")
        : type == QLatin1String("transmit") ? QStringLiteral("Transmit")
        : type == QLatin1String("cw") ? QStringLiteral("CW") : QStringLiteral("Microphone");
}

ProfileRequest::Kind profileKind(const QString& type)
{
    return type == QLatin1String("global") ? ProfileRequest::Kind::Global
        : type == QLatin1String("transmit") ? ProfileRequest::Kind::Transmit
        : type == QLatin1String("cw") ? ProfileRequest::Kind::Cw : ProfileRequest::Kind::Mic;
}
}

// What the shared canon dialog sheet (ModemChrome::Look::Canon: fields,
// buttons, lists, the canon check box) leaves out: tabs as MQTT Settings and
// SpotHub have them; selection ink on the name fields (#6917); the hints and
// result lines; and the name field's right-click menu, a child of the field
// that the sheet's transparent rule would otherwise leave with no panel.
constexpr const char* kProfileManagerRules = R"(
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
QLineEdit {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QLabel#profileHint {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel[resultTone="info"] {
    color: {{color.canon.cyan}};
    font-size: 11px;
}
QLabel[resultTone="error"] {
    color: {{color.accent.danger}};
    font-size: 11px;
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

// How long a Global save may sit unanswered before the result line stops
// claiming it is still in flight. A profile save is a radio-side flash write,
// so it is slower than a routine command; this is generous enough not to fire
// on a merely slow radio, short enough that a dead session does not leave the
// dialog lying about its state indefinitely.
static constexpr int kSaveResponseTimeoutMs = 15000;

ProfileManagerDialog::ProfileManagerDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(QStringLiteral("Profile Manager"), parent, Kind::Workspace),
      m_model(model)
{
    setObjectName(QStringLiteral("ProfileManagerDialog"));
    theme::setContainer(this, QStringLiteral("canon/profileManager"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("ProfileManagerDialogGeometry"));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
             + QString::fromLatin1(kProfileManagerRules);
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(9);
    root->addWidget(makeCanonHeader(QStringLiteral("Profile Manager")));

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

    // CW tab — SmartSDR 4.3.16 added CW profiles; older firmware dims the page.
    QWidget* cwPage = buildProfileTab("cw", model->transmitModel().cwProfileList(),
                                      model->transmitModel().activeCwProfile());
    m_tabs->addTab(cwPage, "CW");
    auto* availability = new ControlAvailabilityRegistry(*model, this);
    availability->registerWidget(cwPage,
        QStringLiteral("CW profiles need a radio running SmartSDR v4.3.16 or later."),
        [](bool, const RadioCapabilities& caps) { return caps.cwProfileControl.has_value(); });

    // Auto-Save tab
    m_tabs->addTab(buildAutoSaveTab(), "Auto-Save");

    root->addWidget(m_tabs, 1);

    // 40 px taller than before for the canon header, and never smaller than
    // the layout needs at this theme's font.
    setMinimumSize(QSize(460, 440).expandedTo(minimumSizeHint()));

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
    connect(&model->transmitModel(), &TransmitModel::cwProfilesChanged, this, [this] {
        refreshTab("cw");
    });
    // The dirty flag only toggles its line; rebuilding the list would drop the
    // operator's selection on every CW setting change.
    connect(&model->transmitModel(), &TransmitModel::cwProfileUnsavedChanged, this,
            [this](bool unsaved) {
        if (m_cwUnsavedLabel) {
            m_cwUnsavedLabel->setVisible(unsaved);
        }
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
    nameEdit->setAccessibleName(QString("%1 profile name").arg(tabLabel(type)));
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
    loadBtn->setObjectName(QString("profileLoadBtn_%1").arg(type));
    deleteBtn->setObjectName(QString("profileDeleteBtn_%1").arg(type));

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
    status->setTextFormat(Qt::PlainText);
    status->setVisible(false);
    vbox->addWidget(status);

    const bool isCw = (type == "cw");
    QPushButton* resetBtn = nullptr;
    if (isCw) {
        // FlexLib 4.3.16 gives CW profiles a reset verb that TX/Mic lack.
        resetBtn = new QPushButton("Reset");
        resetBtn->setObjectName(QStringLiteral("profileResetBtn_cw"));
        resetBtn->setEnabled(false);
        btnRow->insertWidget(2, resetBtn);

        // The radio flags edits to the active CW profile that are not yet saved.
        m_cwUnsavedLabel = new QLabel("The active CW profile has unsaved changes.");
        m_cwUnsavedLabel->setObjectName(QStringLiteral("profileUnsaved_cw"));
        m_cwUnsavedLabel->setVisible(m_model->transmitModel().cwProfileUnsaved());
        vbox->addWidget(m_cwUnsavedLabel);
    }

    if (!isGlobal) {
        auto* note = new QLabel(isCw
            ? QStringLiteral("Create makes a new CW profile; it does not overwrite an\n"
                             "existing one. Reset returns the selected profile to the radio's\n"
                             "built-in CW values, not to the settings of the Default profile.")
            : QStringLiteral("Updates to existing profiles save automatically — enable\n"
                             "Auto-Save (Auto-Save tab) so changes follow the active profile.\n"
                             "Create makes a new profile; it does not overwrite an existing one."));
        note->setObjectName(QStringLiteral("profileHint"));
        note->setWordWrap(true);
        vbox->addWidget(note);
    }

    // Profile list
    auto* list = new QListWidget;
    list->setObjectName(QString("profileList_%1").arg(type));
    list->setAccessibleName(QString("%1 profiles").arg(tabLabel(type)));
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
            [nameEdit, loadBtn, deleteBtn, resetBtn](QListWidgetItem* current, QListWidgetItem*) {
        loadBtn->setEnabled(current != nullptr);
        deleteBtn->setEnabled(current != nullptr);
        if (resetBtn) {
            resetBtn->setEnabled(current != nullptr);
        }
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
        m_model->requestProfile({profileKind(type), ProfileRequest::Action::Load, name}, this);
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
        m_model->requestProfile({profileKind(type), ProfileRequest::Action::Load, name}, this);
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
            m_model->requestProfile(
                {ProfileRequest::Kind::Global, ProfileRequest::Action::Save, name}, this,
                [self, type, name, token](const RadioControlReply& result) {
                    const int code = result.kind == RadioControlReply::Kind::Accepted ? 0 : (result.code == 0 ? -1 : result.code);
                    const QString body = result.message;
                    if (!self) {
                        return;
                    }
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
                    } else if (result.kind == RadioControlReply::Kind::Cancelled
                               || result.kind == RadioControlReply::Kind::Refused) {
                        self->setTabStatus(type, QString("Save of \"%1\" was not completed: %2")
                            .arg(name, body), true);
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
            if (!self) { return; }
            QTimer::singleShot(kSaveResponseTimeoutMs, this,
                               [self, type, name, token] {
                if (!self) {
                    return;
                }
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
            if (exists && type == "cw") {
                setTabStatus(type, QString("A CW profile named \"%1\" already exists.").arg(name), true);
                return;
            }
            if (exists) {
                const QString kind = (type == "transmit") ? "TX" : "Mic";
                if (!m_model->autoSave()) {
                    // Offer Auto-Save inline so the user can act on the
                    // remedy without hunting for the Auto-Save tab.
                    const QPointer<ProfileManagerDialog> self(this);
                    const QPointer<RadioModel> modelGuard(m_model);
                    const RadioControlSession session = m_model->controlSession();
                    ScopedChildWidget<CanonMessageBox> boxOwner(
                        QStringLiteral("Profile already exists"),
                        QString("A %1 profile named \"%2\" already exists.").arg(kind, name), this);
                    CanonMessageBox& box = *boxOwner.get();
                    box.setInformativeText(
                        QString("The radio can't overwrite %1 profiles directly — updates "
                                "are captured by Auto-Save while the profile is active. "
                                "Auto-Save is currently OFF.\n\n"
                                "Would you like to enable Auto-Save now so your changes "
                                "to \"%2\" are captured?").arg(kind, name));
                    box.addButton("Close", CanonMessageBox::Role::Reject);
                    auto* enableBtn = box.addButton("Enable Auto-Save",
                                                    CanonMessageBox::Role::Accept);
                    box.setDefaultButton(enableBtn);
                    box.exec();
                    if (!self || !modelGuard || self->m_model != modelGuard.data() || !boxOwner
                        || modelGuard->controlSession() != session) {
                        return;
                    }
                    if (box.clickedButton() == enableBtn) {
                        // The sibling Auto-Save tab checkbox is wired to
                        // RadioModel::autoSaveChanged below, so the radio's
                        // confirmation of auto_save=1 will sync it.
                        modelGuard->requestProfile({ProfileRequest::Kind::Global,
                            ProfileRequest::Action::AutoSave, {}, true}, self, {}, session);
                    }
                    return;
                }
                ScopedChildWidget<CanonMessageBox> infoOwner(
                    QStringLiteral("Profile already exists"),
                    QString("A %1 profile named \"%2\" already exists.").arg(kind, name), this);
                CanonMessageBox& info = *infoOwner.get();
                info.setInformativeText(
                    QString("The radio cannot overwrite %1 profiles directly. "
                            "Updates are captured by Auto-Save (currently ON) "
                            "while the profile is active.\n\n"
                            "To replace this profile, delete it first and then "
                            "Create it again.").arg(kind));
                info.setDefaultButton(info.addButton("OK", CanonMessageBox::Role::Accept));
                info.exec();
                return;
            }
            const QPointer<ProfileManagerDialog> self(this);
            if (m_model->requestProfile({profileKind(type), ProfileRequest::Action::Create, name}, this)
                == ReceiveDispatch::Unsupported) { return; }
            if (!self) { return; }
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

        const QPointer<ProfileManagerDialog> self(this);
        const QPointer<RadioModel> modelGuard(m_model);
        const RadioControlSession session = m_model->controlSession();
        // Destructive: Cancel is the default, and Cancel, the ✕ or Esc
        // deletes nothing.
        ScopedChildWidget<CanonMessageBox> boxOwner(
            QStringLiteral("Delete Profile"),
            QString("Delete profile \"%1\"?").arg(name), this);
        CanonMessageBox& box = *boxOwner.get();
        auto* cancelBtn = box.addButton("Cancel", CanonMessageBox::Role::Reject);
        auto* deleteConfirm = box.addButton("Delete", CanonMessageBox::Role::Destructive);
        box.setDefaultButton(cancelBtn);
        box.exec();
        if (!self || !modelGuard || self->m_model != modelGuard.data() || !boxOwner
            || modelGuard->controlSession() != session) {
            return;
        }
        if (box.clickedButton() != deleteConfirm) return;

        // Drop the last Save result: leaving it up would keep reporting
        // 'Saved profile "X".' for the profile just deleted (#4362).
        setTabStatus(type, QString(), false);

        modelGuard->requestProfile({profileKind(type), ProfileRequest::Action::Remove, name}, self, {}, session);
    });

    if (resetBtn) {
        connect(resetBtn, &QPushButton::clicked, this, [this, type, list] {
            auto* item = list->currentItem();
            if (!item) return;
            const QString name = item->text();

            const QPointer<ProfileManagerDialog> self(this);
            const QPointer<RadioModel> modelGuard(m_model);
            const RadioControlSession session = m_model->controlSession();
            ScopedChildWidget<CanonMessageBox> boxOwner(
                QStringLiteral("Reset CW Profile"),
                QString("Reset CW profile \"%1\"?").arg(name), this);
            CanonMessageBox& box = *boxOwner.get();
            box.setInformativeText("Its settings return to the radio's built-in CW values. "
                                   "The Default profile is not copied.");
            auto* cancelBtn = box.addButton("Cancel", CanonMessageBox::Role::Reject);
            auto* resetConfirm = box.addButton("Reset", CanonMessageBox::Role::Destructive);
            box.setDefaultButton(cancelBtn);
            box.exec();
            if (!self || !modelGuard || self->m_model != modelGuard.data() || !boxOwner
                || modelGuard->controlSession() != session) {
                return;
            }
            if (box.clickedButton() != resetConfirm) return;

            setTabStatus(type, QString(), false);
            modelGuard->requestProfile({ProfileRequest::Kind::Cw, ProfileRequest::Action::Reset, name},
                                       self, {}, session);
        });
    }

    return page;
}

QWidget* ProfileManagerDialog::buildAutoSaveTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto* desc = new QLabel(
        "When auto-save is enabled, changes to TX and Mic\n"
        "settings are automatically saved to the active profile.");
    desc->setObjectName(QStringLiteral("profileHint"));
    desc->setWordWrap(true);
    vbox->addWidget(desc);
    vbox->addSpacing(10);

    m_autoSaveTx = new QCheckBox("Auto-save profile changes");

    // Read initial state from radio (auto_save in radio status)
    m_autoSaveTx->setChecked(m_model->autoSave());

    connect(m_autoSaveTx, &QCheckBox::toggled, this, [this](bool on) {
        const QPointer<ProfileManagerDialog> self(this);
        if (m_model->requestProfile({ProfileRequest::Kind::Global,
                ProfileRequest::Action::AutoSave, {}, on}, this) == ReceiveDispatch::Unsupported) {
            if (!self) { return; }
            const QSignalBlocker blocker(m_autoSaveTx);
            m_autoSaveTx->setChecked(m_model->autoSave());
        }
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
    // The tone is a property the canon sheet matches, re-polished so the
    // colour follows it and the next theme change.
    status->setProperty("resultTone", error ? "error" : "info");
    status->style()->unpolish(status);
    status->style()->polish(status);
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
    } else if (type == "cw") {
        profiles = m_model->transmitModel().cwProfileList();
        active = m_model->transmitModel().activeCwProfile();
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
