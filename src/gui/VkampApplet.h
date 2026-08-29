#pragma once

#include "core/VkampProtocol.h"

#include <QPushButton>
#include <QTimer>
#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for a VK3AMP RF amplifier -- a sibling of AmpApplet
// (PGXL) and AcomApplet (ACOM), not a variant of either. See
// docs/architecture/vkamp-amplifier-design.md.
//
// Power/Reflected/SWR get permanent gauge rows -- the UDP telemetry frame
// reports all three as independently real fields, same reasoning as
// AcomApplet's own three-gauge layout. Current is a text readout: no
// protocol-defined scale to size a gauge axis against.
class VkampApplet : public QWidget {
    Q_OBJECT

public:
    explicit VkampApplet(QWidget* parent = nullptr);

    // Rescales the forward- and reflected-power gauges to the selected
    // hardware variant's rated output + headroom
    // (Vkamp::meterFullScaleWatts/ratedWatts) -- driven by the Peripherals
    // settings row, see design doc's variant table. Safe to call before or
    // after connect; defaults to W2000.
    void setVariant(Vkamp::Variant variant);

    // Telemetry (UDP)
    void setForwardPower(float watts);
    void setReflectedPower(float watts);
    void setSwr(float swr);
    void setCurrent(float amps);
    // Telemetry has expired -- the amp stopped transmitting and is now silent
    // (VkampConnection::telemetryStalled). Zeroes the four TX-only readouts
    // rather than leaving the last transmit frame on the gauges for the whole
    // idle period. Status fields (temp/volts/band/antenna) are NOT touched:
    // those keep flowing on the TCP link and stay valid.
    void clearTelemetry();

    // Status (TCP)
    void setTemp(float degC);
    void setSupplyVoltage(float volts);
    void setBand(const QString& band);  // read-only display -- see design doc Section 1/3.1
    // Read-only display mirroring the live status, NOT an optimistic latch
    // of the last button clicked -- the amp's own antenna/band table can
    // silently revert a select within ~50ms (design doc Section 3.1).
    void setAntenna(int port);
    void setBypass(bool on);            // also gates the voltage buttons -- see refreshVoltageButtons()
    void setCoolingOverride(bool on);
    void setVoltageLow(bool low);       // which rail the live status reports
    void setFaultCode(int code);        // 0 = no fault -- raw numeric only, see design doc Section 1
    void setConnected(bool connected);  // shows/hides live controls, resets on disconnect

    // Reset hold-to-confirm progress (0 when idle/finished).
    void setResetProgress(double remainingSeconds, bool active);

signals:
    void bypassToggled(bool on);
    void coolingToggled(bool on);
    void antennaSelected(int port);  // 1-3
    void voltageSelected(bool low);
    // Emitted after the applet's own confirm dialog -- MainWindow wiring
    // calls VkampConnection::startReset() in response.
    void resetRequested();

private:
    void updateValueLabels();
    void refreshVoltageButtons();  // enable/active state from m_bypassed + m_voltageLow + m_connected

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_refGauge{nullptr};
    HGauge* m_swrGauge{nullptr};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_refLabel{nullptr};
    QLabel* m_swrLabel{nullptr};
    QLabel* m_currentLabel{nullptr};

    QLabel* m_statusPill{nullptr};

    QLabel* m_tempLabel{nullptr};
    QLabel* m_voltsLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_antennaLabel{nullptr};
    QLabel* m_faultLabel{nullptr};

    QPushButton* m_bypassBtn{nullptr};
    QPushButton* m_coolingBtn{nullptr};
    QPushButton* m_ant1Btn{nullptr};
    QPushButton* m_ant2Btn{nullptr};
    QPushButton* m_ant3Btn{nullptr};
    QPushButton* m_voltLowBtn{nullptr};
    QPushButton* m_voltHighBtn{nullptr};
    QPushButton* m_resetBtn{nullptr};

    QTimer m_labelTimer;
    QTimer* m_peakTimer{nullptr};
    float m_peakFwd{0.0f};

    float m_fwdWatts{0.0f};
    float m_reflectedWatts{0.0f};
    float m_swrVal{1.0f};
    float m_currentAmps{0.0f};
    float m_tempC{0.0f};
    float m_volts{0.0f};
    int m_antennaPort{0};
    int m_faultCode{0};

    bool m_bypassed{false};
    bool m_voltageLow{false};
    bool m_connected{false};
    // A reset hold is running: VkampConnection drops every other command for
    // its duration, so the controls are disabled to match (setResetProgress).
    bool m_resetting{false};

    Vkamp::Variant m_variant{Vkamp::Variant::W2000};

    // setTemp/setSupplyVoltage/setCurrent/setBand/setAntenna arrive at
    // status/telemetry rate -- funneled through the 10Hz m_labelTimer tick
    // (updateValueLabels), same throttle convention as AcomApplet/AmpApplet,
    // instead of repainting and re-announcing accessibility text on every
    // frame.
    bool m_valuesDirty{false};
    bool m_bandDirty{false};
    QString m_pendingBand;
};

}  // namespace AetherSDR
