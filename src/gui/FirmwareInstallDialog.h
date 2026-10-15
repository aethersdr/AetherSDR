#pragma once

#include "gui/CanonWindow.h"

#include <QPointer>
#include <QString>

class QListWidget;
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
// backs up, starts AetherCTR2 and restores backups. Every action is taken on
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
    void syncReleases();
    void showPlaceholder(const QString& text);
    void syncDevice();
    void onInstall();
    void installOverLink();
    void onStart();
    void onBackup();
    void onRestore();
    // Over the link: into the stock firmware, or into download mode.
    void restartDevice(bool bootloader);
    void onFinished(bool ok, const QString& message);
    // Starting an action: its result, or the reason it did not start.
    void started(bool ok, const QString& problem, const QString& doneText);
    QString suggestBackupPath() const;
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
    QPushButton* m_start{nullptr};
    QPushButton* m_backup{nullptr};
    QPushButton* m_restore{nullptr};
    QPushButton* m_toStock{nullptr};
    QPushButton* m_toBootloader{nullptr};
    QPushButton* m_cancel{nullptr};

    QString m_jobDone;       // the success message for the running action
};

} // namespace AetherSDR
