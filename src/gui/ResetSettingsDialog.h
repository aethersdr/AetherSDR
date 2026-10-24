#pragma once

#include "CanonWindow.h"

#include <QStringList>

class QHBoxLayout;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

// Settings → Reset Settings...: the confirmation before anything is removed,
// and the notice when some files could not be. Short-lived CanonWindow
// dialogs (style guide, RFC #6226), modal over their parent.
class ResetSettingsDialog : public CanonWindow {
    Q_OBJECT

public:
    // True only when the operator chose Yes. Cancel is the default button and
    // takes focus, so Return and Escape both leave the settings alone.
    static bool confirm(QWidget* parent, const QString& backupDir,
                        const QStringList& paths);

    // Shown before the app quits when some files could not be removed.
    static void showFailures(QWidget* parent, const QStringList& removed,
                             const QStringList& failed);

private:
    explicit ResetSettingsDialog(const QString& message, QWidget* parent);

    // A heading and a read-only list of file paths; an empty list says so.
    void addPathList(const QString& heading, const QString& accessibleName,
                     const QStringList& paths, const QString& objectName);
    QPushButton* addButton(const QString& text, const QString& objectName);

    QVBoxLayout* m_root{nullptr};
    QHBoxLayout* m_buttons{nullptr};
};

} // namespace AetherSDR
