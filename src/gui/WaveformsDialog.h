#pragma once

#include "PersistentDialog.h"

class QLabel;
class QVBoxLayout;

namespace MasterSDR {

class FlexWaveformModel;

// Non-modal dialog for WFP status and waveform management (File → Waveforms).
// Mirrors the SmartSDR File → Waveforms panel: shows WFP power/ready/IP at the
// top and one row per installed waveform with Restart and Remove/Uninstall
// buttons.  Connects directly to FlexWaveformModel signals so it stays live
// without any MainWindow involvement in refresh.
class WaveformsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit WaveformsDialog(FlexWaveformModel* model, QWidget* parent = nullptr);

private:
    void refreshStatus();
    void refreshWaveformList();

    FlexWaveformModel* m_model;
    QLabel*            m_statusLabel{nullptr};
    QWidget*           m_listContainer{nullptr};
    QVBoxLayout*       m_listLayout{nullptr};
};

} // namespace MasterSDR
