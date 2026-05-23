#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

class QPushButton;
class QLabel;
class QSlider;
class QComboBox;

namespace MasterSDR {

class AtuPreTuneDialog;
class BandPlanManager;
class RadioModel;
class TransmitModel;
class TunerModel;

// TX applet — transmit controls matching the SmartSDR TX panel.
//
// Layout (top to bottom):
//  - Title bar: "TX"
//  - Forward Power horizontal gauge (0–120 W, red > 100 W)
//  - SWR horizontal gauge (1.0–3.0, red > 2.5)
//  - RF Power slider (0–100%)
//  - Tune Power slider (0–100%)
//  - TX Profile dropdown + Success/Byp/Mem indicators
//  - TUNE / MOX / ATU / MEM buttons
//  - Active / Cal / Avail indicators
//  - APD button
class TxApplet : public QWidget {
    Q_OBJECT

public:
    explicit TxApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);
    void setTunerModel(TunerModel* tuner);

    // Wire the dialog dependencies for the ATU right-click menu — band plan
    // for sweep frequency derivation and the RadioModel for slice access
    // and command routing. (#2624)
    void setRadioModel(RadioModel* radio);
    void setBandPlanManager(BandPlanManager* bandPlan);

public slots:
    void updateMeters(float fwdPower, float swr);
    // Capture raw pre-smoothed FWDPWR for PEP peak-hold tick. (#2561)
    void updatePeakPower(float fwdPowerInstant);
    // Reset the peak-hold tick when TX ends so a held peak does not linger
    // across overs. (#2561)
    void setTransmitting(bool tx);
    void setPowerScale(int maxWatts, bool hasAmplifier);

private:
    void buildUI();
    void syncFromModel();
    void syncAtuIndicators();
    // Right-click menu on the ATU button — exposes Pre-tune Bands and
    // Clear ATU Memories. Pre-tune is grayed when MEM is off. (#2624)
    void showAtuContextMenu(const QPoint& pos);
    void openPreTuneDialog();
    void confirmAndClearAtuMemories();
    // Right-click menu on the TUNE button — picks the carrier shape for
    // the *next* tune cycle: "Mono Tone" (single_tone) or "Two Tone".
    // Nothing is persisted — selecting Two Tone is a transient one-shot
    // and the radio reverts to single_tone on its own across power cycles.
    void showTuneContextMenu(const QPoint& pos);

    TransmitModel* m_model{nullptr};
    RadioModel*       m_radioModel{nullptr};
    BandPlanManager*  m_bandPlanMgr{nullptr};
    AtuPreTuneDialog* m_preTuneDialog{nullptr};

    // Frequency at which the ATU last reported a successful tune.
    // Used to gate the second-click → bypass behaviour: a click on the ATU
    // button only sends "atu bypass" when status is Successful/OK *and* the
    // current TX frequency still matches the freq we tuned at.  Any freq
    // change between clicks falls back to "atu start". (#1993)
    double m_atuTunedFreqMhz{-1.0};

    // Gauges (HGauge*)
    QWidget* m_fwdGauge{nullptr};
    QWidget* m_swrGauge{nullptr};

    // Sliders
    QSlider* m_rfPowerSlider{nullptr};
    QSlider* m_tunePowerSlider{nullptr};
    QLabel*  m_rfPowerLabel{nullptr};
    QLabel*  m_tunePowerLabel{nullptr};

    // Profile dropdown
    QComboBox* m_profileCombo{nullptr};

    // ATU status indicators
    QLabel* m_successInd{nullptr};
    QLabel* m_bypInd{nullptr};
    QLabel* m_memInd{nullptr};
    QLabel* m_activeInd{nullptr};
    QLabel* m_calInd{nullptr};
    QLabel* m_availInd{nullptr};

    // Buttons
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_moxBtn{nullptr};
    QPushButton* m_atuBtn{nullptr};
    QPushButton* m_memBtn{nullptr};
    QPushButton* m_apdBtn{nullptr};
    QWidget*     m_apdRow{nullptr};
public:
    void setApdVisible(bool v) { if (m_apdRow) m_apdRow->setVisible(v); }
private:

    bool m_updatingFromModel{false};

    // PEP peak-hold for the FWDPWR gauge — mirrors the SMeterWidget RX
    // peak-hold pattern.  The peak captures the highest pre-smoothed FWDPWR
    // sample, holds for ~2 s, then decays linearly toward the current
    // smoothed reading.  See HGauge::setPeakValue for the tick rendering and
    // SMeterWidget.cpp peak hold for the prior-art ballistics. (#2561)
    float m_smoothedPower{0.0f};
    float m_peakPower{0.0f};
    float m_peakDecayStart{0.0f};
    // Decay rate scaled to the gauge full-scale by setPowerScale so the
    // ~2.5 s visual feel stays consistent across rig classes.  Default
    // matches barefoot (120 W / 2.5 s) for the pre-connect case.
    float m_peakDecayWattsPerSec{48.0f};
    QElapsedTimer m_peakHoldTimer;
    bool m_peakHoldRunning{false};
    QTimer m_peakTick;
};

} // namespace MasterSDR
