#pragma once

#include <QWidget>
#include <QPushButton>
#include <QTimer>

class QLabel;

namespace AetherSDR {

class HGauge;

class AmpApplet : public QWidget {
    Q_OBJECT
public:
    explicit AmpApplet(QWidget* parent = nullptr);

    void setFwdPower(float watts);
    void setSwr(float swr);
    void setTemp(float degC);
    void setTempB(float degC);
    void setDrainCurrent(float amps);
    void setDrainVoltage(float volts);
    void setMainsVoltage(int volts);
    void setState(const QString& state);
    void setFanMode(const QString& mode);  // STANDARD, CONTEST, BROADCAST
    void setMeff(const QString& meff);
    void setDirectConnected(bool direct);

signals:
    void operateToggled(bool on);
    void fanModeChanged(const QString& mode);  // uppercase, ready for sendCommand

private:
    void updateTempLabel();

    // Bargraph gauges
    HGauge*  m_fwdGauge{nullptr};
    HGauge*  m_swrGauge{nullptr};
    HGauge*  m_idGauge{nullptr};

    // Left-side label+value (updated as telemetry arrives)
    QLabel*  m_pwrLabel{nullptr};   // "PWR 1148"
    QLabel*  m_swrLabel{nullptr};   // "SWR 1.2:1"
    QLabel*  m_idLabel{nullptr};    // "Id   39"

    // Right-side info column (one per gauge row)
    QPushButton* m_tempBtn{nullptr}; // "34.7/28.4 C"  (click to toggle C/F)
    QLabel*  m_vddLabel{nullptr};   // "Vdd  50.0 V"  (beside SWR row)
    QLabel*  m_vacLabel{nullptr};   // "Vac   240 V"  (beside Id  row)
    QLabel*  m_sourceLabel{nullptr}; // "● DIRECT" or "● RADIO"
    bool     m_directConnected{false};

    QPushButton* m_fanBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};
    QString      m_fanMode{"STANDARD"};

    // 100 ms timer — updates label text independently of gauge fill rate
    QTimer   m_labelTimer;
    // Peak hold: white tick on fwd gauge, cleared 2.5 s after last new peak
    QTimer*  m_peakTimer{nullptr};
    float    m_peakFwd{0.0f};

    // Cached telemetry values — gauges update every call, labels update at 10 Hz
    float    m_fwdWatts{0.0f};
    float    m_swrVal{1.0f};
    float    m_drainAmps{0.0f};
    float    m_tempA{0.0f};
    float    m_tempB{0.0f};
    bool     m_hasTempA{false};
    bool     m_hasTempB{false};
    bool     m_tempFahrenheit{false};
    int      m_mainsVolts{0};

    void updateValueLabels();
};

} // namespace AetherSDR
