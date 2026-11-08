#pragma once

#include "CanonWindow.h"
#include <QMap>

class QCheckBox;
class QPlainTextEdit;
class QLabel;

namespace AetherSDR {

class RadioModel;

// Help → Support & Diagnostics: per-module diagnostic logging and the log
// file, in a short-lived canon window (RFC #6226, #7051). The static
// fileIssue() / resetSettings() flows live here too but open their own boxes.
class SupportDialog : public CanonWindow {
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
