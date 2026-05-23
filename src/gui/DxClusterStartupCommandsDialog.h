#pragma once

#include "PersistentDialog.h"

class QPlainTextEdit;

namespace MasterSDR {

// Modal editor for the per-instance startup-commands list (#2683).  One
// command per line; the backend (DxClusterClient::sendStartupCommands)
// replays the list after every login, including reconnects.
//
// Use the static edit() helper at the call site — it constructs the
// dialog with the right AppSettings key and runs the modal exec()
// loop.  Two keys exist in practice: "DxClusterStartupCommands" for
// the main cluster tab and "RbnStartupCommands" for the Reverse Beacon
// Network tab, kept independent so operators can configure each
// cluster service separately.
class DxClusterStartupCommandsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit DxClusterStartupCommandsDialog(const QString& title,
                                            const QString& appSettingsKey,
                                            QWidget* parent = nullptr);

    // Convenience launcher.  Opens the editor populated from the given
    // AppSettings key and writes back on OK (no-op on Cancel).
    static void edit(const QString& title,
                     const QString& appSettingsKey,
                     QWidget* parent = nullptr);

private:
    QPlainTextEdit* m_edit{nullptr};
    QString m_key;
};

} // namespace MasterSDR
