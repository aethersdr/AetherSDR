#pragma once

#include "PersistentDialog.h"

#include <QSet>
#include <QString>

class QTableWidget;
class QPushButton;
class QLabel;

namespace AetherSDR {

class RadioModel;

class MultiFlexDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit MultiFlexDialog(RadioModel* model, QWidget* parent = nullptr);

signals:
    void disconnectClientRequested(quint32 handle, const QString& displayName);

private:
    void refresh();

    RadioModel* m_model;
    QTableWidget* m_table;
    QPushButton* m_enableBtn;
    QLabel* m_pttLabel;
    QPushButton* m_pttBtn;
    bool m_refreshing{false};
    // Handles the user has clicked Disconnect for but the radio has not
    // yet evicted from the client list.  Keeps the row's button in the
    // disabled "Disconnecting" state across refresh() rebuilds so a
    // second click can't queue a duplicate disconnect for a handle
    // already on its way out.
    QSet<quint32> m_pendingDisconnects;
};

} // namespace AetherSDR
