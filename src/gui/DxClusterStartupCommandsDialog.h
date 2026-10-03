#pragma once

#include "PersistentDialog.h"

class QPlainTextEdit;

namespace AetherSDR {

// Modal editor for a startup-commands list (#2683), one command per line;
// DxClusterClient::sendStartupCommands replays it after every login. Use
// edit(), which picks the AppSettings key: "DxClusterStartupCommands" (cluster
// tab) or "RbnStartupCommands" (RBN tab), kept independent.
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

} // namespace AetherSDR
