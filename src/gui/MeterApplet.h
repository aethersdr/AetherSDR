#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QCoreApplication>
#include <QWidget>

class QPushButton;

namespace AetherSDR {

class MeterModel;
class HGauge;

// Radio hardware telemetry applet — shows the available PA instrument
// (temperature, or drain current when temperature is unavailable), supply
// voltage, and fan speed. Uses MeterModel's normalized telemetry rather than
// reaching into a family backend.
class MeterApplet : public QWidget {
    Q_OBJECT
public:
    explicit MeterApplet(QWidget* parent = nullptr);

    void setMeterModel(MeterModel* model);
    // The Supply Voltage gauge is dimmed, never hidden, on a radio with no
    // supply rail (HL2); MainWindow registers it with ControlAvailabilityRegistry.
    HGauge* supplyGauge() const { return m_supplyGauge; }
    static bool supplyVoltageAvailable(bool, const RadioCapabilities& caps)
    {
        return caps.hasSupplyVoltageTelemetry;
    }
    static QString supplyVoltageUnavailableReason()
    {
        return QCoreApplication::translate(
            "MeterApplet", "This radio does not report its supply voltage.");
    }
    void setPaInstrumentTelemetryState(bool connected,
                                       bool temperatureAvailable,
                                       bool currentAvailable);
    void setSupplyVoltageTelemetryState(bool connected, bool available);
    void setMainFanTelemetryState(bool connected, bool available);
    void setTransmitting(bool transmitting);

private:
    void resolveIndices();
    void onMeterUpdated(int index, float value);
    void updatePaTempDisplay();
    void updatePaInstrumentState();
    void resetSupplyVoltageDisplay();

    MeterModel* m_model{nullptr};

    HGauge* m_paTempGauge{nullptr};
    HGauge* m_supplyGauge{nullptr};
    HGauge* m_fanGauge{nullptr};

    QPushButton* m_tempUnitBtn{nullptr};

    float m_paTemp{0.0f};
    bool  m_hasPaTemp{false};
    bool  m_tempFahrenheit{false};
    bool  m_paInstrumentConnected{false};
    bool  m_paTemperatureAvailable{false};
    bool  m_paCurrentAvailable{false};
    bool  m_transmitting{false};

    // Lazy-resolved meter index (-1 = not yet found)
    int m_fanIdx{-1};
    bool m_resolved{false};
    bool m_hasMainFanTelemetryState{false};
    bool m_mainFanConnected{false};
    bool m_mainFanAvailable{false};
};

} // namespace AetherSDR
