#pragma once

#include <QWidget>

class QPushButton;
class QLabel;

namespace AetherSDR {

class RadioModel;
class SliceModel;
class MeterSlider;

// DAX Applet — DAX Audio channel meters + gain sliders.
// Displays RX meters for DAX channels 1-8 plus a single TX meter.
class DaxApplet : public QWidget {
    Q_OBJECT

public:
    static constexpr int kChannels = 8;

    explicit DaxApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);

    // Sync Enable button state (called by MainWindow on autostart)
    void setDaxEnabled(bool on);
    void setDaxRxLevel(int channel, float rms);  // channel 1-8
    void setDaxTxLevel(float rms);

signals:
    void daxToggled(bool on);
    void daxRxGainChanged(int channel, float gain);  // 1-8, 0.0–1.0
    void daxTxGainChanged(float gain);

private:
    void buildUI();

    RadioModel* m_model{nullptr};

    QPushButton*  m_daxEnable{nullptr};
    MeterSlider*  m_daxRxMeter[kChannels]{};
    QLabel*       m_daxRxStatus[kChannels]{};
    MeterSlider*  m_daxTxMeter{nullptr};
    QLabel*       m_daxTxStatus{nullptr};
};

} // namespace AetherSDR
