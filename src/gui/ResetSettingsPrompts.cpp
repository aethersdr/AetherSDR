#include "ResetSettingsPrompts.h"
#include "CanonMessageBox.h"

#include <QCoreApplication>
#include <QPushButton>

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

bool confirm(QWidget* parent, const QString& backupDir, const QStringList& paths)
{
    CanonMessageBox box(
        tr("Reset Settings"),
        tr("This will remove AetherSDR's app-specific settings only. "
           "It will not change settings stored on the radio.\n\n"
           "A backup of the current settings is written to %1 before anything is removed.\n\n"
           "AetherSDR will close immediately after reset so these files are not recreated.")
            .arg(backupDir),
        parent);
    box.setObjectName(QStringLiteral("resetSettingsConfirmation"));
    box.setInformativeText(pathList(tr("Files to remove:"), paths)
                           + QStringLiteral("\n\n") + tr("Continue?"));
    // The paths are worth pasting into a bug report.
    box.setTextSelectable(true);

    auto* cancel = box.addButton(tr("Cancel"), CanonMessageBox::Role::Reject);
    cancel->setObjectName(QStringLiteral("resetSettingsCancel"));
    auto* yes = box.addButton(tr("Yes"), CanonMessageBox::Role::Destructive);
    yes->setObjectName(QStringLiteral("resetSettingsConfirm"));
    yes->setAccessibleDescription(
        tr("Back up, then remove AetherSDR's local settings and quit."));
    // A focused Yes must not take Return: only a click confirms.
    yes->setAutoDefault(false);
    box.setDefaultButton(cancel);

    box.exec();
    return box.clickedButton() == yes;
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
