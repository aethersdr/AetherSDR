#pragma once

#include "PersistentDialog.h"

class QLabel;
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
// Docker waveform images via WaveformInstaller.
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
    void updateDStarControls();
    void saveDStarSettings();
    void populateDStarSerialPorts(const QString& preferredPort = {});
    QString selectedDStarSerialPort() const;
    void installWaveformFile(const QString& title, const QString& filter, bool docker);

    RadioModel*        m_radioModel{nullptr};
    QLabel*            m_statusLabel{nullptr};
    QToolButton*       m_installBtn{nullptr};
    QWidget*           m_listContainer{nullptr};
    QVBoxLayout*       m_listLayout{nullptr};
    WaveformInstaller* m_installer{nullptr};

    QLabel*      m_dstarStatusLabel{nullptr};
    QCheckBox*   m_dstarAutoStartCheck{nullptr};
    QLineEdit*   m_dstarExecutableEdit{nullptr};
    QPushButton* m_dstarBrowseBtn{nullptr};
    QToolButton* m_dstarAdvancedBtn{nullptr};
    QWidget*     m_dstarAdvancedPanel{nullptr};
    QLabel*      m_dstarSerialLabel{nullptr};
    QWidget*     m_dstarSerialRow{nullptr};
    QComboBox*   m_dstarSerialCombo{nullptr};
    QToolButton* m_dstarSerialMenuBtn{nullptr};
    QPushButton* m_dstarSerialRefreshBtn{nullptr};
    QPushButton* m_dstarStartStopBtn{nullptr};
};

} // namespace AetherSDR
