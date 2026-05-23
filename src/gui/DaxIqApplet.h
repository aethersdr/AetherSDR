#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QComboBox;
class QProgressBar;

namespace MasterSDR {

class RadioModel;

// DAX IQ Applet — per-channel DAX IQ stream controls.
// For each of 4 IQ channels: rate combo, level meter, and an enable toggle.
class DaxIqApplet : public QWidget {
    Q_OBJECT

public:
    static constexpr int kChannels = 4;

    explicit DaxIqApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);

    void setDaxIqLevel(int channel, float rms);

signals:
    void iqEnableRequested(int channel);
    void iqDisableRequested(int channel);
    void iqRateChanged(int channel, int rate);

private:
    void buildUI();

    RadioModel* m_model{nullptr};

    QPushButton*  m_iqEnable[kChannels]{};
    QComboBox*    m_iqRateCombo[kChannels]{};
    QProgressBar* m_iqMeter[kChannels]{};
};

} // namespace MasterSDR
