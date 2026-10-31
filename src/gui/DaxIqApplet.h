#pragma once

#include <QWidget>
#include <QPointer>
#include <QTimer>
#include <memory>

class QPushButton;
class QLabel;
class QComboBox;
class QProgressBar;

namespace AetherSDR {

class RadioModel;
class ControlAvailabilityRegistry;

// DAX IQ Applet — per-channel DAX IQ stream controls.
// For each of 4 IQ channels: rate combo, level meter, and an enable toggle.
class DaxIqApplet : public QWidget {
    Q_OBJECT

public:
    static constexpr int kChannels = 4;

    explicit DaxIqApplet(QWidget* parent = nullptr);
    ~DaxIqApplet() override;

    void setRadioModel(RadioModel* model);

    void setDaxIqLevel(int channel, float rms);
    void restoreEnabledChannels();

signals:
    void iqEnableRequested(int channel);
    void iqDisableRequested(int channel);
    void iqRateChanged(int channel, int rate);

private:
    void buildUI();

    void refreshRateChoices();
    void syncStreamState(int channel);
    void restoreChannelsNow();

    QPointer<RadioModel> m_model;
    std::unique_ptr<ControlAvailabilityRegistry> m_availability;
    QTimer m_restoreTimer;
    int m_preferredRate[kChannels]{48000, 48000, 48000, 48000};

    QPushButton*  m_iqEnable[kChannels]{};
    QComboBox*    m_iqRateCombo[kChannels]{};
    QProgressBar* m_iqMeter[kChannels]{};
    float         m_iqMeterDb[kChannels]{-70.0f, -70.0f, -70.0f, -70.0f};  // smoothed dBFS per ch
};

} // namespace AetherSDR
