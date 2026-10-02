#pragma once

#include "PersistentDialog.h"

#include <QString>
#include <QStringList>
#include <QList>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;

namespace AetherSDR {

// Non-modal picker for the AppletPanel top-bar buttons. Active column: buttons
// in the bar, the top `favoriteSplit` in the favourites row, the rest in the
// drawer. Hidden column: removed from the bar, their applets disabled
// (Applet_<id>=False). Opened via AppletPanel::openFavoritesPicker() (lazy +
// WA_DeleteOnClose + QPointer); emits layoutAccepted() on OK.
class FavoritesPickerDialog : public PersistentDialog {
    Q_OBJECT

public:
    struct Entry {
        QString id;      // canonical button id (persistence key)
        QString label;   // displayed bar label (e.g. "PHN", "VUDU")
        QString tooltip; // optional longer description (e.g. "Phone")
    };

    FavoritesPickerDialog(const QList<Entry>& allEntries,
                          const QStringList& activeOrder,
                          const QStringList& hiddenIds,
                          int favoriteSplit,
                          QWidget* parent = nullptr);

    QStringList activeOrder() const;
    QStringList hiddenOrder() const;

signals:
    // Emitted when the user clicks OK.  Both lists are emitted so the
    // caller doesn't need to derive one from the other.
    void layoutAccepted(const QStringList& activeOrder,
                        const QStringList& hiddenIds);

private slots:
    void moveSelectedToActive();
    void moveSelectedToHidden();
    void moveActiveUp();
    void moveActiveDown();
    void refreshState();
    void onAccept();

private:
    QListWidgetItem* makeItem(const Entry& e) const;

    int m_favoriteSplit;
    QList<Entry> m_entries;

    QListWidget* m_activeList{nullptr};
    QListWidget* m_hiddenList{nullptr};
    QPushButton* m_addBtn{nullptr};
    QPushButton* m_removeBtn{nullptr};
    QPushButton* m_upBtn{nullptr};
    QPushButton* m_downBtn{nullptr};
    QPushButton* m_okBtn{nullptr};
};

} // namespace AetherSDR
