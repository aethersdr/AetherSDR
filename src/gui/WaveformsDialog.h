#pragma once

#include "CanonWindow.h"
#include <QHash>
#include <QPointer>

class QLabel;
class QAction;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;
class WaveformInstaller;

// Non-modal WFP status and waveform management (Tools -> Waveforms), mirroring
// SmartSDR's File -> Waveforms: WFP power/ready/IP, one row per installed
// waveform with Restart and Remove. Installs .ssdr_waveform packages and Docker
// images via WaveformInstaller; Docker install is gated by live WFP state
// (WaveformInstallGate.h). Takes RadioModel* to construct WaveformInstaller.
class WaveformsDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit WaveformsDialog(RadioModel* model, QWidget* parent = nullptr);

private slots:
    void onInstallLegacyClicked();
    void onInstallDockerClicked();
    void onInstallRemoteAccessClicked();
    // Removes the installed remote-access shim (the container, or on a radio
    // without a Waveform Processor the legacy waveform) and installs the
    // pinned one again, for a computer that lacks the radio's admin token
    // (TailnetShimDialog's Reset).
    void resetRemoteAccess(bool container);
    void onDStarStartStopClicked();
    void onDStarBrowseClicked();

private:
    void refreshStatus();
    void refreshWaveformList();
    void updateInstallButtonState();
    void refreshDStarStatus();
    void refreshDStarConfiguration();
    void updateDStarControls();
    void saveDStarSettings();
    bool saveDStarConfiguration();
    void populateDStarSerialPorts(const QString& preferredPort = {});
    QString selectedDStarSerialPort() const;
    void installWaveformFile(const QString& title,
                             const QString& filter,
                             bool docker,
                             const QString& initialPath = {});
    // Everything after the file is chosen: WFP gate, installer, progress.
    // `quiet` (the radio health tools): no progress window and no result
    // pop-up; the health bar reports the outcome instead.
    void installWaveformPath(bool docker, const QString& path, RadioModel* model,
                             bool quiet = false);
    // Downloads, verifies and installs one remote-access package: the
    // container, or the legacy waveform for a radio without a Waveform
    // Processor.
    void installRemoteAccess(bool container);
    // The bottom bar: "Install radio health tools" mirrors whether the radio
    // lists the agent; the readout polls it while installed.
    void updateHealthBar();
    void onHealthToggled(bool install);
    // Row feedback for Restart / Remove: a restart changes nothing visible,
    // so the row says what was asked and what the radio answered.
    void setRowNotice(const QString& name, const QString& text, const QString& tone, bool pending);
    void onWaveformCommandFinished(const QString& action, const QString& name, bool ok,
                                   const QString& message);

    RadioModel*        m_radioModel{nullptr};
    QLabel*            m_wfpSupportPill{nullptr};
    QLabel*            m_wfpPowerPill{nullptr};
    QLabel*            m_wfpReadyPill{nullptr};
    QLabel*            m_wfpIpPill{nullptr};
    QLabel*            m_connectedRadioNameLabel{nullptr};
    QLabel*            m_connectedRadioSerialLabel{nullptr};
    QToolButton*       m_installBtn{nullptr};
    QAction*           m_installDockerAction{nullptr};
    QAction*           m_installRemoteAccessAction{nullptr};
    struct RowNotice {
        QString text;
        QString tone;      // "pending", "ok" or "error"
        bool    pending{false};
        quint64 serial{0};
    };
    QHash<QString, RowNotice> m_rowNotices;
    quint64 m_rowNoticeSerial{0};
    class TailnetShimDownloader* m_shimDownloader{nullptr};
    class RadioHealthClient* m_healthClient{nullptr};
    QCheckBox*         m_healthCheck{nullptr};
    QLabel*            m_healthLabel{nullptr};           // storage, or a hint/status
    QLabel*            m_healthResourceLabel{nullptr};   // memory and CPU
    QTimer*            m_healthTimer{nullptr};
    bool               m_healthBusy{false};   // an uninstall in flight
    QWidget*           m_listContainer{nullptr};
    QVBoxLayout*       m_listLayout{nullptr};
    WaveformInstaller* m_installer{nullptr};
    // The model m_installer was built for. The guards in installWaveformFile()
    // establish that a model swap across a file picker is reachable, so a
    // cached installer must not keep uploading to the previous radio (#5568
    // review).
    QPointer<RadioModel> m_installerModel;

    QLabel*      m_dstarStatusLabel{nullptr};
    QLabel*      m_dstarDetailLabel{nullptr};
    QCheckBox*   m_dstarAutoStartCheck{nullptr};
    QLineEdit*   m_dstarExecutableEdit{nullptr};
    QLineEdit*   m_dstarMyCallEdit{nullptr};
    QLineEdit*   m_dstarMyCallSuffixEdit{nullptr};
    QLineEdit*   m_dstarUrCallEdit{nullptr};
    QLineEdit*   m_dstarRpt1Edit{nullptr};
    QLineEdit*   m_dstarRpt2Edit{nullptr};
    QLineEdit*   m_dstarMessageEdit{nullptr};
    QPushButton* m_dstarBrowseBtn{nullptr};
    QToolButton* m_dstarAdvancedBtn{nullptr};
    QWidget*     m_dstarAdvancedPanel{nullptr};
    QLabel*      m_dstarSerialLabel{nullptr};
    QWidget*     m_dstarSerialRow{nullptr};
    QComboBox*   m_dstarSerialCombo{nullptr};
    QToolButton* m_dstarSerialMenuBtn{nullptr};
    QPushButton* m_dstarSerialRefreshBtn{nullptr};
    QPushButton* m_dstarStartStopBtn{nullptr};
    bool m_updatingDStarConfiguration{false};
};

} // namespace AetherSDR
