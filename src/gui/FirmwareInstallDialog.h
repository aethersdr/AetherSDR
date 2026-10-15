#pragma once

#include "gui/CanonWindow.h"
#include "core/EspFirmwareCatalog.h"
#include "core/EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>

class QListWidget;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace AetherSDR {

class Ctr2ProxyModel;
class Ctr2ProxyPanel;
class EspFirmwareDownloader;
class EspFirmwareJob;

// The CTR2 window (Settings → Controllers → CTR2...): the CTR2 relay
// (Ctr2ProxyPanel) at the top, and below it the CTR2-Max firmware installer.
// Installs AetherCTR2 on a Lynovation CTR2-Max, beside its stock firmware:
// pick a release from the catalog (EspFirmwareCatalog), then install it. A
// CTR2-Max already running AetherCTR2 is updated in place, through the CTR2
// Proxy's updater (Ctr2FirmwareUpdater): no download mode, and it keeps its
// old firmware if anything goes wrong. One running its stock firmware is put
// in download mode, where the window also backs up, starts AetherCTR2 and
// restores backups. AetherCTR2 is installed only on registered units
// (Ctr2MaxRegistration), as agreed with Lynovation. The layout rules (what
// is protected, what goes where) come from the release's signed manifest
// (EspFirmwareManifest).
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
    void onCatalogReady(const EspFirmwareCatalog& catalog);
    void onProductChosen(int index);
    void showPlaceholder(const QString& text);
    void onReleaseReady(const EspFirmwareManifest& manifest, const QHash<QString, QByteArray>& files);
    void pollDevices();
    void onInstall();
    void installOverLink();
    bool linkUpdateAvailable() const;
    bool busy() const;
    void onStart();
    void onBackup();
    void onRestore();
    // Over the link: into the stock firmware, or into download mode.
    void restartDevice(bool bootloader);
    void onCancel();
    void onProgress(const QString& step, qint64 done, qint64 total);
    void onFinished(bool ok, const QString& message);
    void runJob(int action, const QString& backupPath, const QString& restoreFrom);
    QString suggestBackupPath() const;
    void sync();
    // The status badge: state is the device's mode, detail its tooltip.
    void showStatus(const QString& state, const char* colorToken, const QString& detail);

    QPointer<Ctr2ProxyModel> m_model;
    EspFirmwareDownloader* m_downloader{nullptr};
    EspFirmwareJob* m_job{nullptr};
    QTimer* m_poll{nullptr};

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

    EspFirmwareCatalog m_catalog;
    bool m_haveRelease{false};
    EspFirmwareManifest m_manifest;
    QHash<QString, QByteArray> m_files;
    QString m_port;          // a device in download mode, or empty
    QString m_deviceMac;
    bool m_overLink{false};  // the controller found runs firmware that updates over USB
    QString m_jobDone;       // the success message for the running job
};

} // namespace AetherSDR
