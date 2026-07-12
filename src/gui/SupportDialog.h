#pragma once

#include "PersistentDialog.h"
#include <QMap>

class QCheckBox;
class QPlainTextEdit;
class QLabel;

namespace AetherSDR {

class RadioModel;

class SupportDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SupportDialog(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model) { m_radioModel = model; }

    // Standalone entry points so the Help menu can offer these actions
    // directly, without routing the user through the Support dialog.
    static void fileIssue(QWidget* parent, RadioModel* radioModel);
    static void resetSettings(QWidget* parent);

private slots:
    void refreshLog();
    void clearLog();
    void openLogFolder();
    void enableAll();
    void disableAll();
    void sendToSupport();

private:
    void buildUI();
    void syncCheckboxes();
    QString formatFileSize(qint64 bytes) const;

    RadioModel* m_radioModel{nullptr};
    QMap<QString, QCheckBox*> m_checkboxes;  // category id → checkbox
    QPlainTextEdit* m_logViewer{nullptr};
    QLabel*         m_sizeLabel{nullptr};
};

} // namespace AetherSDR
