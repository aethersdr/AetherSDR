#pragma once

#include "PersistentDialog.h"

#include <QMap>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QTabWidget;

namespace AetherSDR {

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

    // Post-action result line for a tab (#4362).  Empty text hides the label so
    // it costs no layout height until there is something to say.
    void setTabStatus(const QString& type, const QString& text, bool error);

    // In-flight Global save, per tab.  A save is resolved by exactly one of its
    // response callback or its timeout, whichever arrives first; both carry the
    // token they were issued with and no-op unless it is still the pending one.
    // That makes a superseded save (the user hit Save again) silent instead of
    // letting its late reply overwrite the newer one's result line.
    // m_saveSeq only ever increments, so a token is never reused.
    quint64 m_saveSeq{0};
    QMap<QString, quint64> m_pendingSaveToken;   // type -> token, 0 = idle

    RadioModel* m_model;
    QTabWidget* m_tabs;

    // Per-tab widgets (indexed by type: "global", "transmit", "mic")
    struct TabWidgets {
        QLineEdit*   nameEdit;
        QListWidget* list;
        QPushButton* loadBtn;
        QPushButton* saveBtn;
        QPushButton* deleteBtn;
        QLabel*      status;
    };
    QMap<QString, TabWidgets> m_tabWidgets;

    QCheckBox* m_autoSaveTx{nullptr};
};

} // namespace AetherSDR
