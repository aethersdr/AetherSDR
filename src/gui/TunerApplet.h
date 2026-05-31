#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QTimer;

namespace AetherSDR {

class TunerModel;
class MeterModel;

// Tuner applet for the 4o3a Tuner Genius XL (TGXL).
//
// Layout (top to bottom):
//  - Title bar: "TUNER GENIUS XL"
//  - Forward Power horizontal gauge (0–2000 W, red > 1500 W)
//  - SWR horizontal gauge (1.0–3.0, red > 2.5)
//  - C1 / L / C2 relay position bars (0–255)
//  - TUNE + OPERATE/BYPASS/STANDBY buttons
class TunerApplet : public QWidget {
    Q_OBJECT

public:
    explicit TunerApplet(QWidget* parent = nullptr);

    // Attach to a TunerModel (connects signals/slots).
    void setTunerModel(TunerModel* model);

    // Store MeterModel pointer for reading SWR at tune completion.
    void setMeterModel(MeterModel* meter) { m_meter = meter; }

    // Switch Fwd Power gauge scale: barefoot (0–200W), Aurora (0–600W), amplifier (0–2000W).
    void setAmplifierMode(bool hasAmp);  // legacy — calls setPowerScale internally
    void setPowerScale(int maxWatts, bool hasAmplifier);

public slots:
    // Feed forward power (W) and SWR from MeterModel::txMetersChanged.
    void updateMeters(float fwdPower, float swr);

private:
    void buildUI();
    void syncFromModel();
    void cycleOperateState();
    void updateAntennaButtons(int antA);
    void updateValueLabels();

    TunerModel* m_model{nullptr};
    MeterModel* m_meter{nullptr};

    // Gauges (custom-painted inner widgets)
    QWidget* m_fwdGauge{nullptr};
    QWidget* m_swrGauge{nullptr};

    // Row labels that show live numeric values ("PWR 987", "SWR 1.2:1")
    QLabel*  m_pwrLabel{nullptr};
    QLabel*  m_swrLabel{nullptr};
    QTimer*  m_labelClearTimer{nullptr};  // holds label visible after power drops
    bool     m_labelShowing{false};

    // Relay bars
    QWidget* m_c1Bar{nullptr};
    QWidget* m_lBar{nullptr};
    QWidget* m_c2Bar{nullptr};

    // Buttons
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};

    // Antenna switch buttons (TGXL 3x1)
    QPushButton* m_ant1Btn{nullptr};
    QPushButton* m_ant2Btn{nullptr};
    QPushButton* m_ant3Btn{nullptr};
    QWidget*     m_antContainer{nullptr};

    // Meter values (updated by updateMeters)
    float m_fwdPower{0.0f};
    float m_swr{1.0f};

    // Peak hold for fwd gauge
    QTimer* m_peakTimer{nullptr};
    float   m_peakFwd{0.0f};

    // Relay values (updated from model)
    int m_relayC1{0};
    int m_relayL{0};
    int m_relayC2{0};

    // Track tuning state for SWR result flash
    bool m_wasTuning{false};
    bool m_postTuneCapture{false};  // true during post-tune settling window
    float m_tuneSwr{1.0f};   // last non-1.00 SWR seen while tuning
    QTimer* m_postTuneTimer{nullptr};
};

} // namespace AetherSDR
