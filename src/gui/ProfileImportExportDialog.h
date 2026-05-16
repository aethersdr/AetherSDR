#pragma once

#include "PersistentDialog.h"

#include "core/ProfileTransfer.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;

namespace AetherSDR {

class ProfileTransfer;
class RadioModel;

class ProfileImportExportDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit ProfileImportExportDialog(RadioModel* model, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* buildExportPage();
    QWidget* buildImportPage();
    ExportSelection currentExportSelection() const;
    QString defaultExportPath() const;
    QString defaultExportFileName() const;
    bool canTransfer(QString* reason = nullptr) const;
    bool confirmImport();

    void chooseExportPath();
    void chooseImportPath();
    void startExport();
    void startImport();
    void setTransferring(bool transferring);
    void updateControls();
    void updateSelectAllFromOptions();
    void setStatus(const QString& text);

    RadioModel* m_model{nullptr};
    ProfileTransfer* m_transfer{nullptr};

    QTabWidget* m_tabs{nullptr};

    QCheckBox* m_selectAllExport{nullptr};
    QCheckBox* m_globalProfiles{nullptr};
    QCheckBox* m_txProfiles{nullptr};
    QCheckBox* m_micProfiles{nullptr};
    QCheckBox* m_memories{nullptr};
    QCheckBox* m_preferences{nullptr};
    QCheckBox* m_tnf{nullptr};
    QCheckBox* m_xvtr{nullptr};
    QCheckBox* m_usbCables{nullptr};

    QLineEdit* m_exportPath{nullptr};
    QLineEdit* m_importPath{nullptr};
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_progress{nullptr};
    QPushButton* m_exportButton{nullptr};
    QPushButton* m_importButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QPushButton* m_closeButton{nullptr};
};

} // namespace AetherSDR
