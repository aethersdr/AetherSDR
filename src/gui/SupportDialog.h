#pragma once

#include <QDialog>
#include <QMap>

class QCheckBox;
class QPlainTextEdit;
class QLabel;

namespace MasterSDR {

class RadioModel;

class SupportDialog : public QDialog {
    Q_OBJECT

public:
    explicit SupportDialog(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model) { m_radioModel = model; }

private slots:
    void refreshLog();
    void clearLog();
    void openLogFolder();
    void resetSettings();
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

} // namespace MasterSDR
