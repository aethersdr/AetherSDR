#pragma once

#include "PersistentDialog.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;
class WaveformInstaller;

// Non-modal dialog for WFP status and waveform management (File → Waveforms).
// Mirrors the SmartSDR File → Waveforms panel: shows WFP power/ready/IP at the
// top and one row per installed waveform with Restart and Remove/Uninstall
// buttons.  An Install… button at the top-right lets the user upload a Docker
// waveform image (.tar.gz) via WaveformInstaller.
//
// Takes RadioModel* so it can construct WaveformInstaller (which needs
// sendCmdPublic and radioAddress()) while still connecting to FlexWaveformModel
// signals for live list updates.
class WaveformsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit WaveformsDialog(RadioModel* model, QWidget* parent = nullptr);

private slots:
    void onInstallClicked();

private:
    void refreshStatus();
    void refreshWaveformList();
    void updateInstallButtonState();

    RadioModel*        m_radioModel{nullptr};
    QLabel*            m_statusLabel{nullptr};
    QPushButton*       m_installBtn{nullptr};
    QWidget*           m_listContainer{nullptr};
    QVBoxLayout*       m_listLayout{nullptr};
    WaveformInstaller* m_installer{nullptr};
};

} // namespace AetherSDR
