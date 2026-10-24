#include "ResetSettingsDialog.h"
#include "CanonIndicators.h"
#include "ModemChrome.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

ResetSettingsDialog::ResetSettingsDialog(const QString& message, QWidget* parent)
    : CanonWindow(tr("Reset Settings"), parent)
{
    setObjectName(QStringLiteral("resetSettingsDialog"));
    // Paths run long; wide enough that a typical one fits on a line.
    resize(560, 420);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon);
    });

    m_root = new QVBoxLayout(bodyWidget());
    m_root->setContentsMargins(14, 12, 14, 14);
    m_root->setSpacing(8);
    m_root->addWidget(makeCanonHeader(tr("Reset Settings")));

    auto* text = new QLabel(message);
    text->setObjectName(QStringLiteral("resetSettingsMessage"));
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_root->addWidget(text);

    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(6);
    m_buttons->addStretch(1);
}

void ResetSettingsDialog::addPathList(const QString& heading, const QString& accessibleName,
                                      const QStringList& paths, const QString& objectName)
{
    auto* label = new QLabel(heading);
    label->setObjectName(QStringLiteral("SectionLabel"));
    m_root->addWidget(label);

    auto* list = new QListWidget;
    list->setObjectName(objectName);
    list->setAccessibleName(accessibleName);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    if (paths.isEmpty()) {
        list->addItem(tr("None"));
    } else {
        list->addItems(paths);
    }
    m_root->addWidget(list, 1);
}

QPushButton* ResetSettingsDialog::addButton(const QString& text, const QString& objectName)
{
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    m_buttons->addWidget(button);
    return button;
}

bool ResetSettingsDialog::confirm(QWidget* parent, const QString& backupDir,
                                  const QStringList& paths)
{
    ResetSettingsDialog dlg(
        tr("This will remove AetherSDR's app-specific settings only. "
           "It will not change settings stored on the radio.\n\n"
           "A backup of the current settings is written to %1 before anything is removed.\n\n"
           "AetherSDR will close immediately after reset so these files are not recreated.")
            .arg(backupDir),
        parent);
    dlg.addPathList(tr("Files to remove"), tr("Files Reset Settings will remove"),
                    paths, QStringLiteral("resetSettingsFiles"));
    dlg.m_root->addWidget(new QLabel(tr("Continue?")));

    auto* cancel = dlg.addButton(tr("Cancel"), QStringLiteral("resetSettingsCancel"));
    auto* yes = dlg.addButton(tr("Yes"), QStringLiteral("resetSettingsConfirm"));
    yes->setAccessibleDescription(
        tr("Back up, then remove AetherSDR's local settings and quit."));
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(yes, &QPushButton::clicked, &dlg, &QDialog::accept);
    yes->setAutoDefault(false);
    cancel->setDefault(true);
    dlg.m_root->addLayout(dlg.m_buttons);
    cancel->setFocus();

    return dlg.exec() == QDialog::Accepted;
}

void ResetSettingsDialog::showFailures(QWidget* parent, const QStringList& removed,
                                       const QStringList& failed)
{
    ResetSettingsDialog dlg(
        tr("Some files could not be removed.\n\n"
           "AetherSDR will now close to avoid rewriting settings."),
        parent);
    dlg.addPathList(tr("Failed"), tr("Files that could not be removed"),
                    failed, QStringLiteral("resetSettingsFailed"));
    dlg.addPathList(tr("Removed"), tr("Files that were removed"),
                    removed, QStringLiteral("resetSettingsRemoved"));

    auto* ok = dlg.addButton(tr("OK"), QStringLiteral("resetSettingsOk"));
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    ok->setDefault(true);
    dlg.m_root->addLayout(dlg.m_buttons);
    ok->setFocus();

    dlg.exec();
}

} // namespace AetherSDR
