#pragma once

#include <QStringList>

class QWidget;

namespace AetherSDR::ResetSettingsPrompts {

// Settings → Reset Settings...: the confirmation before anything is removed,
// and the notice when some files could not be. CanonMessageBox dialogs (style
// guide, RFC #6226), modal over their parent.

// True only when the operator clicks Yes. Cancel is the default button and takes
// focus, and Yes never takes Return, so Return and Escape leave the settings alone.
bool confirm(QWidget* parent, const QString& backupDir, const QStringList& paths);

// Shown before the app quits when some files could not be removed.
void showFailures(QWidget* parent, const QStringList& removed, const QStringList& failed);

} // namespace AetherSDR::ResetSettingsPrompts
