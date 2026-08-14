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

    // Bound the operator-facing RX row count to the radio's slice capacity
    // (Principle I): a 6300 exposes 2, a 6700 eight. Rows above n are HIDDEN,
    // not destroyed — kChannels stays the allocation. Fed by MainWindow from
    // RadioModel::maxSlices(); see #4854 review.
    void setMaxDaxChannels(int n);

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
    int m_maxDaxChannels{kChannels};  // radio slice capacity (FlexLib table)

    QPushButton*  m_daxEnable{nullptr};
    QWidget*      m_daxRxRow[kChannels]{};  // per-channel row container — hidden to gate to maxSlices()
    MeterSlider*  m_daxRxMeter[kChannels]{};
    QLabel*       m_daxRxStatus[kChannels]{};
    MeterSlider*  m_daxTxMeter{nullptr};
    QLabel*       m_daxTxStatus{nullptr};
};

} // namespace AetherSDR
