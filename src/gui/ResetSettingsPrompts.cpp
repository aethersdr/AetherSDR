#include "ResetSettingsPrompts.h"
#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "CanonWindow.h"
#include "ModemChrome.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR::ResetSettingsPrompts {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ResetSettingsPrompts", text);
}

// One path per line under its heading; an empty list says so.
QString pathList(const QString& heading, const QStringList& paths)
{
    return heading + QLatin1Char('\n')
         + (paths.isEmpty() ? tr("None") : QStringLiteral("- ") + paths.join(QStringLiteral("\n- ")));
}

} // namespace

// Its own CanonWindow rather than a CanonMessageBox: the files to remove sit in
// a nested list, which the message box (text only) has no place for.
bool confirm(QWidget* parent, const QString& backupDir, const QStringList& paths)
{
    CanonWindow dlg(tr("Reset Settings"), parent);
    dlg.setObjectName(QStringLiteral("resetSettingsConfirmation"));
    // Paths run long; wide enough that a typical one fits on a line.
    dlg.resize(560, 420);
    // The buttons match CanonMessageBox's, as the failure notice uses it.
    applyCanonSheet(dlg.bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
               + QStringLiteral(
                   "QPushButton { padding: 6px 18px; }"
                   "QPushButton:default { color: {{color.canon.cyan}}; border-color: {{color.canon.cyan}}; }"
                   "QPushButton[role=\"destructive\"] { color: {{color.canon.sparkGold}}; }");
    });

    auto* root = new QVBoxLayout(dlg.bodyWidget());
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(8);
    root->addWidget(makeCanonHeader(tr("Reset Settings")));

    auto* text = new QLabel(
        tr("This will remove AetherSDR's app-specific settings only. "
           "It will not change settings stored on the radio.\n\n"
           "A backup of the current settings is written to %1 before anything is removed.\n\n"
           "AetherSDR will close immediately after reset so these files are not recreated.")
            .arg(backupDir));
    text->setObjectName(QStringLiteral("resetSettingsMessage"));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(text);

    auto* heading = new QLabel(tr("Files to remove"));
    heading->setObjectName(QStringLiteral("SectionLabel"));
    root->addWidget(heading);

    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("resetSettingsFiles"));
    list->setAccessibleName(tr("Files Reset Settings will remove"));
    // Selectable, so the paths can be copied into a bug report.
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->addItems(paths.isEmpty() ? QStringList{tr("None")} : paths);
    root->addWidget(list, 1);

    root->addWidget(new QLabel(tr("Continue?")));

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("resetSettingsCancel"));
    auto* yes = new QPushButton(tr("Yes"));
    yes->setObjectName(QStringLiteral("resetSettingsConfirm"));
    yes->setProperty("role", QStringLiteral("destructive"));
    yes->setAccessibleDescription(
        tr("Back up, then remove AetherSDR's local settings and quit."));
    buttons->addWidget(cancel);
    buttons->addWidget(yes);
    root->addLayout(buttons);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(yes, &QPushButton::clicked, &dlg, &QDialog::accept);
    // A focused Yes must not take Return: only a click confirms.
    yes->setAutoDefault(false);
    cancel->setDefault(true);
    cancel->setFocus();

    return dlg.exec() == QDialog::Accepted;
}

void showFailures(QWidget* parent, const QStringList& removed, const QStringList& failed)
{
    CanonMessageBox box(tr("Reset Settings"),
                        tr("Some files could not be removed.\n\n"
                           "AetherSDR will now close to avoid rewriting settings."),
                        parent);
    box.setObjectName(QStringLiteral("resetSettingsFailures"));
    box.setInformativeText(pathList(tr("Failed:"), failed) + QStringLiteral("\n\n")
                           + pathList(tr("Removed:"), removed));
    box.setTextSelectable(true);

    auto* ok = box.addButton(tr("OK"), CanonMessageBox::Role::Accept);
    ok->setObjectName(QStringLiteral("resetSettingsOk"));
    box.setDefaultButton(ok);
    box.exec();
}

} // namespace AetherSDR::ResetSettingsPrompts
