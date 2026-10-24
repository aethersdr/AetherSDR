#pragma once

#include <QFrame>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

class QButtonGroup;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace AetherSDR {

class FirmwareStager;
class FirmwareUploader;
class RadioModel;

// The Updates window's radio section: the connected radio's firmware against
// the releases its vendor publishes, laid out as one lane per major line, and
// the install itself. Install… downloads the chosen release, checks its MD5,
// extracts the firmware and uploads it (FirmwareStager, FirmwareUploader);
// Open Installer… does the same from a file the operator downloaded.
// Experimental, and says so. A radio the client cannot update, or none, dims
// the section with the reason.
class FirmwareUpdateCard : public QFrame {
    Q_OBJECT

public:
    explicit FirmwareUpdateCard(RadioModel* model, QWidget* parent = nullptr);

    // Downloading, staging or uploading.
    bool isBusy() const;

    // Asked before the window closes. Idle: true. While working: a prompt
    // naming what closing does at this phase; true only if the operator
    // confirms, after the work is cancelled.
    bool confirmStop(QWidget* parent);

    // The published releases (newest first, as FirmwareStager reports them)
    // as one lane per major line: newest line first, each lane's releases
    // oldest to newest.
    static QList<QPair<int, QStringList>> firmwareLanes(const QStringList& releasesNewestFirst);

    // The release Install starts on: the newest in the radio's own line when
    // it is newer than what is installed; otherwise empty.
    static QString preselectedRelease(const QStringList& releasesNewestFirst,
                                      const QString& installedVersion);

signals:
    void busyChanged(bool busy);

private:
    friend class UpdatesWindowTestAccess;

    enum class Step { Idle, Prepare, Upload, Confirm };

    void refreshAvailability();
    void requestReleases();
    void showReleases(const QStringList& releases);
    QString selectedRelease() const;
    void syncSelection();
    void startInstall();
    void openInstaller();
    void uploadStaged(const QString& path, bool confirmFirst, bool unverifiedDownload = false);
    void ensureUploader();
    void setStep(Step step);
    void setStatus(const QString& text, const QString& tone);
    void setBusy(bool busy);

    QPointer<RadioModel> m_model;
    FirmwareStager* m_stager{nullptr};
    FirmwareUploader* m_uploader{nullptr};
    bool m_staging{false};
    bool m_busy{false};
    bool m_closePromptOpen{false};
    bool m_uploadAfterStage{false};
    QStringList m_releases;

    QLabel* m_radioName{nullptr};
    QLabel* m_radioDetail{nullptr};
    QLabel* m_pill{nullptr};
    QWidget* m_lanes{nullptr};
    QVBoxLayout* m_lanesLayout{nullptr};
    QButtonGroup* m_releaseGroup{nullptr};
    QLabel* m_description{nullptr};
    QPushButton* m_install{nullptr};
    QPushButton* m_openInstaller{nullptr};
    QPushButton* m_notes{nullptr};
    QPushButton* m_cancel{nullptr};
    QWidget* m_steps{nullptr};
    QList<QLabel*> m_stepTiles;
    QProgressBar* m_progress{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_caution{nullptr};
};

} // namespace AetherSDR
