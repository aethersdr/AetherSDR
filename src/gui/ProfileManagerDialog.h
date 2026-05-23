#pragma once

#include "PersistentDialog.h"

#include <QMap>

class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QTabWidget;

namespace MasterSDR {

class RadioModel;

class ProfileManagerDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit ProfileManagerDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    QWidget* buildProfileTab(const QString& type, const QStringList& profiles,
                             const QString& active);
    QWidget* buildAutoSaveTab();
    void refreshTab(const QString& type);

    RadioModel* m_model;
    QTabWidget* m_tabs;

    // Per-tab widgets (indexed by type: "global", "transmit", "mic")
    struct TabWidgets {
        QLineEdit*   nameEdit;
        QListWidget* list;
        QPushButton* loadBtn;
        QPushButton* saveBtn;
        QPushButton* deleteBtn;
    };
    QMap<QString, TabWidgets> m_tabWidgets;

    QCheckBox* m_autoSaveTx{nullptr};
};

} // namespace MasterSDR
