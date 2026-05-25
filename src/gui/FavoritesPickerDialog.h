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

// Non-modal, frameless-aware picker for the AppletPanel top-bar buttons.
//
// Two columns:
//   * Active — every button shown in the bar.  Top `favoriteSplit`
//     entries appear in the favourites row; the rest appear in the
//     drawer.  Reordering within this column rearranges both halves
//     accordingly.
//   * Hidden — buttons removed from the bar entirely.  Their applets
//     are disabled (Applet_<id>=False) when moved here.
//
// Derives from PersistentDialog so it inherits the canonical title-bar
// + frameless-chrome + geometry-persistence boilerplate
// (docs/style/dialog-patterns.md).
//
// Lifetime: opened via AppletPanel::openFavoritesPicker() with the
// standard lazy-construct + WA_DeleteOnClose + QPointer slot pattern.
// Emits layoutAccepted() on OK with the new order + hidden lists.
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
