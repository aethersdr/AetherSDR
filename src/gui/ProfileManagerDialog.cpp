#include "ProfileManagerDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCloseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QRect>

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
    "QCheckBox { color: #c8d8e8; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }";

ProfileManagerDialog::ProfileManagerDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Profile Manager");
    setMinimumSize(460, 400);
    setStyleSheet(kDialogStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("Profile Manager"), this);
    outer->addWidget(m_titleBar);

    auto* bodyWidget = new QWidget(this);
    auto* root = new QVBoxLayout(bodyWidget);
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(9);
    m_bodyLayout = root;
    outer->addWidget(bodyWidget, 1);

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

    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");

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
    restoreGeometryFromSettings();
}

void ProfileManagerDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }

    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_bodyLayout) {
        m_bodyLayout->setContentsMargins(9, on ? 7 : 9, 9, 9);
    }
    if (wasVisible) {
        show();
    }
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

    // Buttons: Load, Save, Delete
    auto* btnRow = new QHBoxLayout;
    auto* loadBtn = new QPushButton("Load");
    auto* saveBtn = new QPushButton("Save");
    auto* deleteBtn = new QPushButton("Delete");

    loadBtn->setEnabled(false);
    deleteBtn->setEnabled(false);

    btnRow->addWidget(loadBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addWidget(deleteBtn);
    vbox->addLayout(btnRow);

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

    // Save button — saves current state under the name in the text field
    // (or the selected list item if field is empty)
    connect(saveBtn, &QPushButton::clicked, this, [this, type, nameEdit, list] {
        QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) {
            auto* item = list->currentItem();
            if (item) name = item->text();
        }
        if (name.isEmpty()) return;

        if (type == "global")
            m_model->sendCommand(QString("profile global save \"%1\"").arg(name));
        else if (type == "transmit")
            m_model->sendCommand(QString("profile transmit create \"%1\"").arg(name));
        else if (type == "mic")
            m_model->sendCommand(QString("profile mic create \"%1\"").arg(name));

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

// Geometry persistence — AppSettings serializes via XML strings, so window
// geometry round-trips through base64 to preserve binary data.  Matches the
// project convention used by MainWindow{Geometry,State}.  Save fires on
// close (guaranteed persistence on clean exit) AND on each move/resize
// (crash-resilient: if AetherSDR is force-quit, the last known position is
// already in AppSettings).
void ProfileManagerDialog::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    s.setValue("ProfileManagerDialogGeometry", saveGeometry().toBase64());
}

void ProfileManagerDialog::restoreGeometryFromSettings()
{
    const QString geomB64 = AppSettings::instance()
        .value("ProfileManagerDialogGeometry").toString();
    if (geomB64.isEmpty()) return;
    // Restore triggers move/resize events; flag those as restore-driven so
    // they don't immediately overwrite the value we just loaded.
    m_restoringGeometry = true;
    restoreGeometry(QByteArray::fromBase64(geomB64.toLatin1()));
    m_restoringGeometry = false;
}

void ProfileManagerDialog::closeEvent(QCloseEvent* event)
{
    saveGeometryToSettings();
    // Flush to disk on close.  Move/resize saves are in-memory only; the
    // close-time save is the canonical persistence point.
    AppSettings::instance().save();
    QDialog::closeEvent(event);
}

void ProfileManagerDialog::moveEvent(QMoveEvent* event)
{
    QDialog::moveEvent(event);
    if (!m_restoringGeometry) saveGeometryToSettings();
}

void ProfileManagerDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (!m_restoringGeometry) saveGeometryToSettings();
}

} // namespace AetherSDR
