#pragma once

#include <QDialog>
#include <QMap>

class QVBoxLayout;
class QTabWidget;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;

namespace AetherSDR {

class RadioModel;

class ProfileManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProfileManagerDialog(RadioModel* model, QWidget* parent = nullptr);
    void setFramelessMode(bool on);

private:
    QWidget* buildProfileTab(const QString& type, const QStringList& profiles,
                             const QString& active);
    QWidget* buildAutoSaveTab();
    void refreshTab(const QString& type);

    RadioModel* m_model;
    QTabWidget* m_tabs;
    QWidget* m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};

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

} // namespace AetherSDR
