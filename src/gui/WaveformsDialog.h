#pragma once

#include "PersistentDialog.h"

class QLabel;
class QAction;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;
class WaveformInstaller;

// Non-modal dialog for WFP status and waveform management (File → Waveforms).
// Mirrors the SmartSDR File → Waveforms panel: shows WFP power/ready/IP at the
// top and one row per installed waveform with Restart and Remove/Uninstall
// buttons.  The install menu supports legacy .ssdr_waveform packages and
// Docker waveform images via WaveformInstaller; Docker install is gated by the
// radio's WFP license-feature status plus live WFP power/ready state.
//
// Takes RadioModel* so it can construct WaveformInstaller (which needs
// sendCmdPublic and radioAddress()) while still connecting to FlexWaveformModel
// signals for live list updates.
class WaveformsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit WaveformsDialog(RadioModel* model, QWidget* parent = nullptr);

private slots:
    void onInstallLegacyClicked();
    void onInstallDockerClicked();
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

    RadioModel*        m_radioModel{nullptr};
    QLabel*            m_wfpSupportPill{nullptr};
    QLabel*            m_wfpPowerPill{nullptr};
    QLabel*            m_wfpReadyPill{nullptr};
    QLabel*            m_wfpIpPill{nullptr};
    QLabel*            m_connectedRadioNameLabel{nullptr};
    QLabel*            m_connectedRadioSerialLabel{nullptr};
    QToolButton*       m_installBtn{nullptr};
    QAction*           m_installDockerAction{nullptr};
    QWidget*           m_listContainer{nullptr};
    QVBoxLayout*       m_listLayout{nullptr};
    WaveformInstaller* m_installer{nullptr};

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
