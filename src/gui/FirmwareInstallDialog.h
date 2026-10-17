#pragma once

#include "gui/CanonWindow.h"

#include <QPointer>
#include <QString>

class QListWidget;
class QTabWidget;
class QLabel;
class QProgressBar;
class QPushButton;

namespace AetherSDR {

class Ctr2ProxyModel;
class Ctr2ProxyPanel;
class FirmwareInstallModel;

// The CTR2 window (Settings → Controllers → CTR2...): the CTR2 relay
// (Ctr2ProxyPanel, over Ctr2ProxyModel) at the top, and below it the
// CTR2-Max firmware installer (over FirmwareInstallModel), which installs
// AetherCTR2 on a Lynovation CTR2-Max beside its stock firmware. A CTR2-Max
// already running AetherCTR2 is updated in place over the USB cable; one
// running its stock firmware is put in download mode, where the window also
// backs up, starts AetherCTR2 and restores backups. The CTR2-Max's own
// firmware, downloaded by the operator from lynovation.com, is installed
// from its Lynovation tab (Open File…); AetherCTR2 from its own tab. Every action is taken on
// the device the operator confirmed (FirmwareInstallModel::Target).
class FirmwareInstallDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit FirmwareInstallDialog(Ctr2ProxyModel* model, QWidget* parent = nullptr);
    ~FirmwareInstallDialog() override;

protected:
    // Not while a job runs: the device would be left part-written.
    void closeEvent(QCloseEvent* event) override;
    void reject() override;   // Escape

private:
    // The two firmware lists: each tab's firmware, then the unit's backups.
    void syncLists();
    void syncText();   // what the chosen row of each list is
    QString selectedBackup(const QListWidget* list) const;
    QString backupText(const QString& path) const;
    void syncDevice();
    void onInstall();
    void installOverLink();
    // Lynovation's CTR2-Max firmware, from a file the operator downloaded.
    void onOpenFile();
    void installStockFirmware();
    // Into a FirmwareInstallModel::Firmware (as int: the model header stays
    // out of this one).
    void restartInto(int which);
    void onFinished(bool ok, const QString& message);
    // Starting an action: its result, or the reason it did not start.
    void started(bool ok, const QString& problem, const QString& doneText);
    // For a unit on its stock firmware (stock): tells the operator to put it
    // in download mode. False if they cancel.
    bool askDownloadMode(bool stock);
    // Restores the backup at path on the unit connected now, once confirmed.
    void confirmRestore(const QString& path);
    void sync();
    // The status badge: state is the device's mode, detail its tooltip.
    void showStatus(const QString& state, const char* colorToken, const QString& detail);

    QPointer<Ctr2ProxyModel> m_model;
    FirmwareInstallModel* m_fw{nullptr};

    QListWidget* m_product{nullptr};
    QLabel* m_release{nullptr};
    QLabel* m_badge{nullptr};
    QLabel* m_step{nullptr};
    QProgressBar* m_bar{nullptr};
    Ctr2ProxyPanel* m_relay{nullptr};
    QPushButton* m_install{nullptr};
    QPushButton* m_toAetherCtr2{nullptr};
    QPushButton* m_toStock{nullptr};
    QPushButton* m_toBootloader{nullptr};
    QPushButton* m_cancel{nullptr};
    QTabWidget* m_tabs{nullptr};   // AetherCTR2, Lynovation
    QPushButton* m_openFile{nullptr};
    QListWidget* m_fileList{nullptr};   // the opened file's firmware, as m_product
    QLabel* m_fileText{nullptr};
    QPushButton* m_installFile{nullptr};
    bool m_installAfterDownload{false};   // Install pressed on a release to download

    QString m_jobDone;       // the success message for the running action
};

} // namespace AetherSDR
